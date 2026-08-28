#include "optimizer/memory_manager.hpp"
#include "core/logger.hpp"
#include "core/config.hpp"
#include "core/utils.hpp"
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace surface_optimizer {

namespace {

static const wchar_t* const DEFAULT_SYSTEM_ALLOWLIST[] = {
    L"smss.exe", L"csrss.exe", L"wininit.exe", L"services.exe",
    L"lsass.exe", L"svchost.exe", L"fontdrvhost.exe", L"dwm.exe",
    L"explorer.exe", L"sihost.exe", L"taskhostw.exe", L"audiodg.exe",
    L"msmpeng.exe", L"securityhealthservice.exe", L"surface_optimizer.exe"
};

std::wstring to_lower_str(std::wstring str) {
    std::transform(str.begin(), str.end(), str.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return str;
}

} // anonymous namespace

MemoryManager& MemoryManager::get_instance() {
    static MemoryManager instance;
    return instance;
}

MemoryManager::MemoryManager() {
    m_grace_period_ms = 10000;
}

bool MemoryManager::initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) {
        return true;
    }

    // Enable memory management privileges if available in token
    Utils::enable_privilege(SE_PROF_SINGLE_PROCESS_NAME);
    Utils::enable_privilege(SE_INCREASE_QUOTA_NAME);

    // Dynamically resolve NtSetSystemInformation from ntdll.dll
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        FARPROC proc = GetProcAddress(hNtdll, "NtSetSystemInformation");
        m_nt_set_system_information = reinterpret_cast<pfnNtSetSystemInformation>(reinterpret_cast<void*>(proc));
    }

    if (m_nt_set_system_information) {
        LOG_INFO(L"MemoryManager: Resolved NtSetSystemInformation (NTAPI Class 80 available).");
    } else {
        LOG_WARN(L"MemoryManager: Failed to resolve NtSetSystemInformation. Standby purge will be unavailable.");
    }

    m_initialized = true;
    LOG_INFO(L"MemoryManager initialized successfully.");
    return true;
}

MemoryStats MemoryManager::get_memory_stats() const {
    MemoryStats stats{};
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);

    if (GlobalMemoryStatusEx(&mem_status)) {
        stats.memory_load_percent = mem_status.dwMemoryLoad;
        stats.total_phys_bytes = mem_status.ullTotalPhys;
        stats.avail_phys_bytes = mem_status.ullAvailPhys;
    } else {
        LOG_WARN(L"GlobalMemoryStatusEx failed: " + Utils::get_last_error_message());
    }

    return stats;
}

bool MemoryManager::purge_standby_list_internal() {
    if (!m_nt_set_system_information) {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            FARPROC proc = GetProcAddress(hNtdll, "NtSetSystemInformation");
            m_nt_set_system_information = reinterpret_cast<pfnNtSetSystemInformation>(reinterpret_cast<void*>(proc));
        }
    }

    if (!m_nt_set_system_information) {
        LOG_WARN(L"purge_standby_list: NtSetSystemInformation is not available.");
        return false;
    }

    // Try full standby purge (Command = 2: MemoryPurgeStandbyList)
    int cmd = static_cast<int>(SystemMemoryListCommand::MemoryPurgeStandbyList);
    NTSTATUS status = m_nt_set_system_information(80, &cmd, sizeof(cmd));

    if (status == 0) { // STATUS_SUCCESS
        LOG_INFO(L"Full Standby List purged successfully (MemoryPurgeStandbyList).");
        return true;
    }

    // If unprivileged (STATUS_PRIVILEGE_NOT_HELD 0xC0000061), fallback to low-priority purge (Command = 3)
    cmd = static_cast<int>(SystemMemoryListCommand::MemoryPurgeLowPriorityStandbyList);
    status = m_nt_set_system_information(80, &cmd, sizeof(cmd));
    if (status == 0) {
        LOG_INFO(L"Low-Priority Standby List purged successfully (MemoryPurgeLowPriorityStandbyList).");
        return true;
    }

    LOG_DEBUG(L"NtSetSystemInformation standby purge returned status: 0x" + std::to_wstring(status));
    return false;
}

bool MemoryManager::purge_standby_list() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return purge_standby_list_internal();
}

bool MemoryManager::trim_process_working_set(uint32_t pid) {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
        return false;
    }

    if (is_pid_protected(pid)) {
        LOG_DEBUG(L"trim_process_working_set: PID " + std::to_wstring(pid) + L" is protected. Trimming skipped.");
        return false;
    }

    // Try opening with PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION
    HANDLE hProcess = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }

    if (!hProcess) {
        return false;
    }

    BOOL res = EmptyWorkingSet(hProcess);
    CloseHandle(hProcess);
    return res != FALSE;
}

void MemoryManager::on_foreground_process_changed(uint32_t pid, const std::wstring& /*image_name*/) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_current_foreground_pid = pid;
    if (pid != 0) {
        m_recent_foreground_history[pid] = std::chrono::steady_clock::now();
    }
    cleanup_expired_focus_history_locked();
}

void MemoryManager::cleanup_expired_focus_history_locked() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = m_recent_foreground_history.begin(); it != m_recent_foreground_history.end(); ) {
        if (it->first == m_current_foreground_pid) {
            ++it;
            continue;
        }
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
        if (elapsed_ms > m_grace_period_ms) {
            it = m_recent_foreground_history.erase(it);
        } else {
            ++it;
        }
    }
}

bool MemoryManager::is_image_allowlisted(const std::wstring& image_name) const {
    if (image_name.empty()) return true;

    std::wstring lower_name = to_lower_str(image_name);

    // Check default core allowlist
    for (const auto* allowed : DEFAULT_SYSTEM_ALLOWLIST) {
        if (lower_name == allowed) {
            return true;
        }
    }

    // Check configuration allowlist
    const auto& config = Config::get_instance();
    for (const auto& item : config.governor.allowlist) {
        if (lower_name == to_lower_str(item)) {
            return true;
        }
    }

    return false;
}

bool MemoryManager::is_pid_protected_locked(uint32_t pid) const {
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
        return true;
    }

    // Active foreground PID protection
    if (pid == m_current_foreground_pid) {
        return true;
    }

    // Recent foreground focus grace period protection
    auto it = m_recent_foreground_history.find(pid);
    if (it != m_recent_foreground_history.end()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
        if (elapsed_ms < m_grace_period_ms) {
            return true;
        }
    }

    // Allowlist check
    std::wstring proc_name = Utils::get_process_name_by_pid(pid);
    if (is_image_allowlisted(proc_name)) {
        return true;
    }

    return false;
}

bool MemoryManager::is_pid_protected(uint32_t pid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return is_pid_protected_locked(pid);
}

void MemoryManager::set_recent_focus_grace_period_ms(uint32_t ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_grace_period_ms = ms;
}

uint32_t MemoryManager::get_current_foreground_pid() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_current_foreground_pid;
}

TrimResult MemoryManager::get_last_trim_result() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_trim_result;
}

uint64_t MemoryManager::get_last_trim_timestamp() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_trim_epoch_ms;
}

TrimResult MemoryManager::optimize_memory(uint32_t current_foreground_pid, bool force_standby_purge) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (current_foreground_pid != 0) {
        m_current_foreground_pid = current_foreground_pid;
        m_recent_foreground_history[current_foreground_pid] = std::chrono::steady_clock::now();
    }
    cleanup_expired_focus_history_locked();

    TrimResult result{};
    std::vector<DWORD> pids(2048);
    DWORD bytes_returned = 0;

    if (EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &bytes_returned)) {
        DWORD count = bytes_returned / sizeof(DWORD);

        for (DWORD i = 0; i < count; ++i) {
            DWORD pid = pids[i];
            if (is_pid_protected_locked(pid)) {
                continue;
            }

            HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (!hProc) {
                hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            }

            if (!hProc) {
                continue;
            }

            PROCESS_MEMORY_COUNTERS_EX pmc_before{};
            pmc_before.cb = sizeof(pmc_before);
            uint64_t ws_before = 0;
            if (GetProcessMemoryInfo(hProc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc_before), sizeof(pmc_before))) {
                ws_before = pmc_before.WorkingSetSize;
            }

            if (EmptyWorkingSet(hProc)) {
                result.processes_trimmed++;
                PROCESS_MEMORY_COUNTERS_EX pmc_after{};
                pmc_after.cb = sizeof(pmc_after);
                if (GetProcessMemoryInfo(hProc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc_after), sizeof(pmc_after))) {
                    if (ws_before > pmc_after.WorkingSetSize) {
                        result.bytes_freed += (ws_before - pmc_after.WorkingSetSize);
                    }
                }
            }

            CloseHandle(hProc);
        }
    }

    if (force_standby_purge) {
        result.standby_list_purged = purge_standby_list_internal();
    }

    m_last_trim_result = result;
    m_last_trim_time = std::chrono::steady_clock::now();
    m_last_trim_epoch_ms = Utils::get_current_time_millis();

    double freed_mb = static_cast<double>(result.bytes_freed) / (1024.0 * 1024.0);
    LOG_INFO(L"Memory optimization completed: " + std::to_wstring(result.processes_trimmed) +
             L" background processes trimmed, " + std::to_wstring(static_cast<uint64_t>(freed_mb)) +
             L" MB freed, Standby purged: " + (result.standby_list_purged ? L"YES" : L"NO"));

    return result;
}

void MemoryManager::on_housekeeping(uint32_t current_foreground_pid, bool is_ac) {
    const auto& config = Config::get_instance();

    if (is_ac && !config.memory.trim_idle_processes_on_ac) return;
    if (!is_ac && !config.memory.trim_idle_processes_on_dc) return;

    auto stats = get_memory_stats();
    bool high_pressure = (stats.memory_load_percent >= config.memory.pressure_threshold_percent);
    bool critical_pressure = (stats.memory_load_percent >= 85);

    auto now = std::chrono::steady_clock::now();
    auto elapsed_sec = (m_last_trim_time.time_since_epoch().count() == 0)
        ? 999999
        : std::chrono::duration_cast<std::chrono::seconds>(now - m_last_trim_time).count();

    uint32_t cooldown = critical_pressure
        ? (config.memory.trim_interval_seconds / 2)
        : config.memory.trim_interval_seconds;
    if (cooldown < 10) cooldown = 10;

    if (high_pressure && static_cast<uint32_t>(elapsed_sec) >= cooldown) {
        LOG_INFO(L"Memory load threshold reached (" + std::to_wstring(stats.memory_load_percent) +
                 L"% >= " + std::to_wstring(config.memory.pressure_threshold_percent) +
                 L"%). Triggering background memory optimization...");
        optimize_memory(current_foreground_pid, config.memory.enable_standby_purge);
    }
}

void MemoryManager::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recent_foreground_history.clear();
    m_initialized = false;
    LOG_INFO(L"MemoryManager shutdown complete.");
}

} // namespace surface_optimizer
