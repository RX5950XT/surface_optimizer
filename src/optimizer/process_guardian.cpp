#include "optimizer/process_guardian.hpp"
#include "optimizer/cpu_reserve.hpp"
#include "core/logger.hpp"
#include "core/config.hpp"
#include "core/utils.hpp"
#include <psapi.h>
#include <algorithm>
#include <cwctype>
#include <sstream>
#include <iomanip>
#include <unordered_map>

namespace surface_optimizer {

namespace {

// Well-known system processes that must never be throttled
static const wchar_t* const SYSTEM_CRITICAL_ALLOWLIST[] = {
    L"system", L"smss.exe", L"csrss.exe", L"wininit.exe", L"services.exe",
    L"lsass.exe", L"svchost.exe", L"fontdrvhost.exe", L"dwm.exe",
    L"explorer.exe", L"sihost.exe", L"taskhostw.exe", L"audiodg.exe",
    L"msmpeng.exe", L"securityhealthservice.exe", L"surface_optimizer.exe",
    L"winlogon.exe", L"conhost.exe", L"spoolsv.exe", L"searchhost.exe",
    L"runtimebroker.exe", L"shellexperiencehost.exe", L"startmenuexperiencehost.exe",
    L"textinputhost.exe", L"ctfmon.exe", L"dllhost.exe", L"wmiprvse.exe"
};

std::wstring to_lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

} // anonymous namespace

ProcessGuardian& ProcessGuardian::get_instance() {
    static ProcessGuardian instance;
    return instance;
}

ProcessGuardian::ProcessGuardian() = default;

bool ProcessGuardian::initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return true;

    // Take initial baseline snapshot
    auto snapshots = take_snapshot();
    for (auto& snap : snapshots) {
        m_prev_snapshots[snap.pid] = std::move(snap);
    }

    m_initialized = true;
    LOG_INFO(L"ProcessGuardian initialized. Baseline captured for " +
             std::to_wstring(m_prev_snapshots.size()) +
             L" processes. Foreground HighQoS / background EcoQoS armed.");
    return true;
}

void ProcessGuardian::enable_enforcement() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        return;
    }
    m_enforce = true;
    discover_os_reserve_masks();
    GuardianStats dummy{};
    apply_os_cpu_reserve(m_prev_snapshots, dummy);
}

void ProcessGuardian::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    restore_restricted_affinity();
    m_prev_snapshots.clear();
    m_cpu_hog_ticks.clear();
    m_mem_leak_ticks.clear();
    m_throttled_pids.clear();
    m_qos_eco.clear();
    m_affinity_original.clear();
    m_enforce = false;
    m_initialized = false;
    LOG_INFO(L"ProcessGuardian shutdown complete.");
}

void ProcessGuardian::on_foreground_process_changed(uint32_t pid, const std::wstring& /*image_name*/) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t prev = m_current_foreground_pid;
    m_current_foreground_pid = pid;
    if (!m_initialized) {
        return;
    }

    if (pid != 0 && pid != 4 && pid != GetCurrentProcessId()) {
        if (apply_high_qos(pid)) {
            m_qos_eco[pid] = false;
        }
        if (m_enforce) {
            std::wstring name = Utils::get_process_name_by_pid(pid);
            if (should_restrict_off_os_core(is_process_allowlisted(name), false, false)) {
                restrict_off_os_core(pid);
            }
        }
    }

    const auto& config = Config::get_instance();
    if (!config.governor.enable_eco_qos || prev == 0 || prev == pid) {
        return;
    }
    std::wstring prev_name = Utils::get_process_name_by_pid(prev);
    if (is_protected(prev, prev_name)) {
        return;
    }
    if (apply_eco_qos(prev)) {
        m_qos_eco[prev] = true;
    }
}

bool ProcessGuardian::is_process_allowlisted(const std::wstring& image_name) const {
    if (image_name.empty()) return true;
    std::wstring lower_name = to_lower(image_name);

    for (const auto* name : SYSTEM_CRITICAL_ALLOWLIST) {
        if (lower_name == name) return true;
    }

    const auto& config = Config::get_instance();
    for (const auto& item : config.governor.allowlist) {
        if (lower_name == to_lower(item)) return true;
    }

    return false;
}

bool ProcessGuardian::is_protected(uint32_t pid, const std::wstring& image_name) const {
    // System idle (0) and System (4) are always protected
    if (pid == 0 || pid == 4) return true;

    // Our own process
    if (pid == GetCurrentProcessId()) return true;

    // Active foreground app
    if (pid == m_current_foreground_pid) return true;

    // Allowlisted system processes
    if (is_process_allowlisted(image_name)) return true;

    return false;
}

std::vector<ProcessSnapshot> ProcessGuardian::take_snapshot() const {
    std::vector<ProcessSnapshot> results;
    std::vector<DWORD> pids(4096);
    DWORD bytes_returned = 0;

    if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &bytes_returned)) {
        LOG_WARN(L"ProcessGuardian: EnumProcesses failed: " + Utils::get_last_error_message());
        return results;
    }

    DWORD count = bytes_returned / sizeof(DWORD);
    auto now = std::chrono::steady_clock::now();

    for (DWORD i = 0; i < count; ++i) {
        DWORD pid = pids[i];
        if (pid == 0 || pid == 4) continue;

        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProc) {
            hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        }
        if (!hProc) continue;

        ProcessSnapshot snap;
        snap.pid = pid;
        snap.sample_time = now;

        // Get image name
        wchar_t name_buf[MAX_PATH] = {};
        DWORD name_len = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, name_buf, &name_len)) {
            std::wstring full_path(name_buf, name_len);
            auto pos = full_path.find_last_of(L"\\/");
            snap.image_name = (pos != std::wstring::npos) ? full_path.substr(pos + 1) : full_path;
        }

        // Get CPU times
        FILETIME create_time{}, exit_time{}, kernel_time{}, user_time{};
        if (GetProcessTimes(hProc, &create_time, &exit_time, &kernel_time, &user_time)) {
            ULARGE_INTEGER kt, ut;
            kt.LowPart = kernel_time.dwLowDateTime;
            kt.HighPart = kernel_time.dwHighDateTime;
            ut.LowPart = user_time.dwLowDateTime;
            ut.HighPart = user_time.dwHighDateTime;
            snap.kernel_time = kt.QuadPart;
            snap.user_time = ut.QuadPart;
        }

        // Get working set
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(hProc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            snap.working_set_bytes = pmc.WorkingSetSize;
        }

        CloseHandle(hProc);
        results.push_back(std::move(snap));
    }

    return results;
}

double ProcessGuardian::compute_cpu_percent(const ProcessSnapshot& prev, const ProcessSnapshot& curr) const {
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        curr.sample_time - prev.sample_time).count();
    if (elapsed <= 0) return 0.0;

    uint64_t prev_total = prev.kernel_time + prev.user_time;
    uint64_t curr_total = curr.kernel_time + curr.user_time;

    if (curr_total < prev_total) return 0.0;

    // CPU times are in 100-ns units. Convert elapsed to 100-ns units too.
    double elapsed_100ns = static_cast<double>(elapsed) * 10.0;
    double cpu_time_delta = static_cast<double>(curr_total - prev_total);

    // Single-core percentage (we want per-process CPU load)
    return (cpu_time_delta / elapsed_100ns) * 100.0;
}

int64_t ProcessGuardian::compute_ws_delta(const ProcessSnapshot& prev, const ProcessSnapshot& curr) const {
    return static_cast<int64_t>(curr.working_set_bytes) - static_cast<int64_t>(prev.working_set_bytes);
}

bool ProcessGuardian::set_execution_speed_throttle(uint32_t pid, bool enable_eco) const {
    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!hProc) {
        return false;
    }

    // ProcessPowerThrottling = 4, PROCESS_POWER_THROTTLING_EXECUTION_SPEED = 1
    struct {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    } throttle_state;

    throttle_state.Version = 1;
    throttle_state.ControlMask = 1;
    throttle_state.StateMask = enable_eco ? 1u : 0u;

    BOOL result = SetProcessInformation(
        hProc,
        static_cast<PROCESS_INFORMATION_CLASS>(4),
        &throttle_state,
        sizeof(throttle_state)
    );
    CloseHandle(hProc);
    return result != FALSE;
}

bool ProcessGuardian::apply_eco_qos(uint32_t pid) {
    if (!set_execution_speed_throttle(pid, true)) {
        LOG_DEBUG(L"ProcessGuardian: Failed to apply EcoQoS to PID " + std::to_wstring(pid) +
                  L": " + Utils::get_last_error_message());
        return false;
    }
    LOG_DEBUG(L"ProcessGuardian: Applied EcoQoS to PID " + std::to_wstring(pid));
    return true;
}

bool ProcessGuardian::apply_high_qos(uint32_t pid) {
    if (!set_execution_speed_throttle(pid, false)) {
        LOG_DEBUG(L"ProcessGuardian: Failed to apply HighQoS to PID " + std::to_wstring(pid) +
                  L": " + Utils::get_last_error_message());
        return false;
    }
    LOG_DEBUG(L"ProcessGuardian: Applied HighQoS to PID " + std::to_wstring(pid));
    return true;
}

void ProcessGuardian::apply_foreground_background_qos(
    const std::unordered_map<uint32_t, ProcessSnapshot>& curr_map,
    GuardianStats& stats) {
    const auto& config = Config::get_instance();
    size_t newly_high = 0;
    size_t newly_eco = 0;

    for (auto it = m_qos_eco.begin(); it != m_qos_eco.end(); ) {
        if (curr_map.find(it->first) == curr_map.end()) {
            it = m_qos_eco.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& [pid, curr] : curr_map) {
        if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
            continue;
        }
        if (pid == m_current_foreground_pid) {
            auto it = m_qos_eco.find(pid);
            if (it != m_qos_eco.end() && !it->second) {
                continue;
            }
            if (apply_high_qos(pid)) {
                m_qos_eco[pid] = false;
                ++newly_high;
            }
            continue;
        }
        if (!config.governor.enable_eco_qos) {
            continue;
        }
        if (is_protected(pid, curr.image_name)) {
            continue;
        }
        auto it = m_qos_eco.find(pid);
        if (it != m_qos_eco.end() && it->second) {
            continue;
        }
        if (apply_eco_qos(pid)) {
            m_qos_eco[pid] = true;
            ++newly_eco;
        }
    }

    for (const auto& [pid, eco] : m_qos_eco) {
        if (eco) {
            stats.ecoqos_active++;
        } else {
            stats.highqos_active++;
        }
    }

    if (newly_high > 0 || newly_eco > 0) {
        LOG_INFO(L"ProcessGuardian: QoS updated (High +" + std::to_wstring(newly_high) +
                 L", Eco +" + std::to_wstring(newly_eco) + L")");
    }

    apply_os_cpu_reserve(curr_map, stats);
}

void ProcessGuardian::discover_os_reserve_masks() {
    m_os_core_mask = 0;
    m_user_affinity_mask = 0;

    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        LOG_WARN(L"ProcessGuardian: cannot read processor topology; OS CPU reserve disabled.");
        return;
    }

    std::vector<uint8_t> buf(len);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(buf.data());
    if (!GetLogicalProcessorInformation(info, &len)) {
        LOG_WARN(L"ProcessGuardian: GetLogicalProcessorInformation failed: " +
                 Utils::get_last_error_message());
        return;
    }

    DWORD_PTR first_core = 0;
    uint32_t cores = 0;
    const DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    for (DWORD i = 0; i < count; ++i) {
        if (info[i].Relationship != RelationProcessorCore) {
            continue;
        }
        if (cores == 0) {
            first_core = info[i].ProcessorMask;
        }
        ++cores;
    }

    if (!should_reserve_os_core(cores) || first_core == 0) {
        LOG_INFO(L"ProcessGuardian: OS CPU reserve skipped (physical cores=" +
                 std::to_wstring(cores) + L").");
        return;
    }

    DWORD_PTR proc_mask = 0;
    DWORD_PTR sys_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) || sys_mask == 0) {
        LOG_WARN(L"ProcessGuardian: GetProcessAffinityMask failed; OS CPU reserve disabled.");
        return;
    }

    m_os_core_mask = first_core;
    m_user_affinity_mask = static_cast<DWORD_PTR>(
        user_affinity_mask(static_cast<uint64_t>(sys_mask), static_cast<uint64_t>(first_core)));
    std::wstringstream mask_log;
    mask_log << std::hex << L"ProcessGuardian: OS CPU reserve armed. os_core=0x"
             << m_os_core_mask << L" user=0x" << m_user_affinity_mask
             << L" (user processes kept off the first physical core).";
    LOG_INFO(mask_log.str());
}

bool ProcessGuardian::restrict_off_os_core(uint32_t pid) {
    if (m_user_affinity_mask == 0 || m_os_core_mask == 0) {
        return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc) {
        hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }
    if (!hProc) {
        return false;
    }

    DWORD_PTR proc_mask = 0;
    DWORD_PTR sys_mask = 0;
    if (!GetProcessAffinityMask(hProc, &proc_mask, &sys_mask) || proc_mask == 0) {
        CloseHandle(hProc);
        return false;
    }

    if (!affinity_uses_os_core(static_cast<uint64_t>(proc_mask),
                               static_cast<uint64_t>(m_os_core_mask))) {
        CloseHandle(hProc);
        return false;
    }

    if (m_affinity_original.find(pid) == m_affinity_original.end()) {
        m_affinity_original[pid] = proc_mask;
    }

    const BOOL ok = SetProcessAffinityMask(hProc, m_user_affinity_mask);
    CloseHandle(hProc);
    if (!ok) {
        LOG_DEBUG(L"ProcessGuardian: SetProcessAffinityMask failed PID " +
                  std::to_wstring(pid) + L": " + Utils::get_last_error_message());
        return false;
    }
    return true;
}

void ProcessGuardian::apply_os_cpu_reserve(
    const std::unordered_map<uint32_t, ProcessSnapshot>& curr_map,
    GuardianStats& stats) {
    if (!m_enforce || m_user_affinity_mask == 0) {
        return;
    }

    for (auto it = m_affinity_original.begin(); it != m_affinity_original.end(); ) {
        if (curr_map.find(it->first) == curr_map.end()) {
            it = m_affinity_original.erase(it);
        } else {
            ++it;
        }
    }

    size_t newly = 0;
    const uint32_t self = GetCurrentProcessId();
    for (const auto& [pid, curr] : curr_map) {
        const bool system_pid = (pid == 0 || pid == 4);
        if (!should_restrict_off_os_core(is_process_allowlisted(curr.image_name),
                                         pid == self, system_pid)) {
            continue;
        }
        const bool had = m_affinity_original.count(pid) != 0;
        if (restrict_off_os_core(pid) && !had && m_affinity_original.count(pid) != 0) {
            ++newly;
        }
    }

    stats.affinity_restricted = m_affinity_original.size();
    if (newly > 0) {
        LOG_INFO(L"ProcessGuardian: OS CPU reserve applied to " +
                 std::to_wstring(newly) + L" processes (active=" +
                 std::to_wstring(m_affinity_original.size()) + L").");
    }
}

void ProcessGuardian::restore_restricted_affinity() {
    if (m_affinity_original.empty()) {
        return;
    }
    size_t restored = 0;
    for (const auto& [pid, original] : m_affinity_original) {
        HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
        if (!hProc) {
            continue;
        }
        if (SetProcessAffinityMask(hProc, original)) {
            ++restored;
        }
        CloseHandle(hProc);
    }
    LOG_INFO(L"ProcessGuardian: restored affinity on " + std::to_wstring(restored) +
             L" processes.");
}

bool ProcessGuardian::demote_priority(uint32_t pid) {
    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    // Set to BELOW_NORMAL_PRIORITY_CLASS (0x00004000)
    BOOL result = SetPriorityClass(hProc, BELOW_NORMAL_PRIORITY_CLASS);
    CloseHandle(hProc);

    if (result) {
        LOG_INFO(L"ProcessGuardian: Demoted priority of PID " + std::to_wstring(pid) +
                 L" to BELOW_NORMAL.");
    }

    return result != FALSE;
}

void ProcessGuardian::cleanup_stale_throttles() {
    for (auto it = m_throttled_pids.begin(); it != m_throttled_pids.end(); ) {
        // Check if the process still exists
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it->first);
        if (!hProc) {
            it = m_throttled_pids.erase(it);
            continue;
        }
        CloseHandle(hProc);
        ++it;
    }
}

GuardianStats ProcessGuardian::on_housekeeping(uint32_t current_foreground_pid) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        return {};
    }

    m_current_foreground_pid = current_foreground_pid;
    cleanup_stale_throttles();

    const auto& config = Config::get_instance();
    double cpu_threshold = config.governor.cpu_hog_threshold_percent;
    uint32_t sustain_ticks = config.governor.cpu_hog_sustain_seconds /
                             (config.daemon.housekeeping_interval_ac_ms / 1000);
    if (sustain_ticks < 2) sustain_ticks = 2;

    GuardianStats stats{};
    auto current_snapshots = take_snapshot();
    stats.processes_scanned = current_snapshots.size();

    // Build current snapshot map
    std::unordered_map<uint32_t, ProcessSnapshot> curr_map;
    for (auto& snap : current_snapshots) {
        curr_map[snap.pid] = snap;
    }

    // Clean stale ticks for processes that no longer exist
    for (auto it = m_cpu_hog_ticks.begin(); it != m_cpu_hog_ticks.end(); ) {
        if (curr_map.find(it->first) == curr_map.end()) {
            it = m_cpu_hog_ticks.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_mem_leak_ticks.begin(); it != m_mem_leak_ticks.end(); ) {
        if (curr_map.find(it->first) == curr_map.end()) {
            it = m_mem_leak_ticks.erase(it);
        } else {
            ++it;
        }
    }

    apply_foreground_background_qos(curr_map, stats);

    // Analyze each process
    for (const auto& [pid, curr] : curr_map) {
        if (is_protected(pid, curr.image_name)) {
            if (is_process_allowlisted(curr.image_name)) {
                stats.skipped_allowlist++;
            }
            if (pid == m_current_foreground_pid) {
                stats.skipped_foreground++;
            }
            // Reset ticks for protected processes
            m_cpu_hog_ticks.erase(pid);
            m_mem_leak_ticks.erase(pid);
            continue;
        }

        // Already throttled - skip further analysis
        if (m_throttled_pids.count(pid)) continue;

        auto prev_it = m_prev_snapshots.find(pid);
        if (prev_it == m_prev_snapshots.end()) continue;

        // CPU hog detection
        double cpu_pct = compute_cpu_percent(prev_it->second, curr);
        if (cpu_pct > cpu_threshold) {
            m_cpu_hog_ticks[pid]++;
            if (m_cpu_hog_ticks[pid] >= sustain_ticks) {
                stats.cpu_hogs_detected++;
                LOG_WARN(L"ProcessGuardian: CPU hog detected - PID=" + std::to_wstring(pid) +
                         L" (" + curr.image_name + L") at " +
                         std::to_wstring(static_cast<int>(cpu_pct)) + L"% for " +
                         std::to_wstring(m_cpu_hog_ticks[pid]) + L" ticks.");

                ThrottleRecord rec;
                rec.pid = pid;
                rec.image_name = curr.image_name;
                rec.throttled_at = std::chrono::steady_clock::now();
                rec.last_cpu_percent = cpu_pct;
                rec.last_ws_bytes = curr.working_set_bytes;

                if (config.governor.enable_eco_qos) {
                    rec.eco_qos_applied = apply_eco_qos(pid);
                }
                if (config.governor.enable_priority_demotion) {
                    rec.priority_demoted = demote_priority(pid);
                }

                if (rec.eco_qos_applied || rec.priority_demoted) {
                    m_throttled_pids[pid] = rec;
                    stats.throttled_count++;
                }
                m_cpu_hog_ticks.erase(pid);
            }
        } else {
            // Reset counter if CPU usage drops back below threshold
            m_cpu_hog_ticks.erase(pid);
        }

        // Memory leak detection: continuous WS growth > 100MB
        int64_t ws_delta = compute_ws_delta(prev_it->second, curr);
        constexpr int64_t LEAK_THRESHOLD_BYTES = 100LL * 1024 * 1024; // 100MB
        if (ws_delta > 0 && curr.working_set_bytes > static_cast<uint64_t>(LEAK_THRESHOLD_BYTES)) {
            m_mem_leak_ticks[pid]++;
            if (m_mem_leak_ticks[pid] >= sustain_ticks) {
                stats.mem_leaks_detected++;
                LOG_WARN(L"ProcessGuardian: Memory leak suspected - PID=" + std::to_wstring(pid) +
                         L" (" + curr.image_name + L") WS=" +
                         std::to_wstring(curr.working_set_bytes / (1024 * 1024)) + L" MB, delta=+" +
                         std::to_wstring(ws_delta / (1024 * 1024)) + L" MB.");

                if (!m_throttled_pids.count(pid)) {
                    ThrottleRecord rec;
                    rec.pid = pid;
                    rec.image_name = curr.image_name;
                    rec.throttled_at = std::chrono::steady_clock::now();
                    rec.last_cpu_percent = 0;
                    rec.last_ws_bytes = curr.working_set_bytes;

                    if (config.governor.enable_eco_qos) {
                        rec.eco_qos_applied = apply_eco_qos(pid);
                    }
                    if (config.governor.enable_priority_demotion) {
                        rec.priority_demoted = demote_priority(pid);
                    }

                    if (rec.eco_qos_applied || rec.priority_demoted) {
                        m_throttled_pids[pid] = rec;
                        stats.throttled_count++;
                    }
                }
                m_mem_leak_ticks.erase(pid);
            }
        } else if (ws_delta <= 0) {
            m_mem_leak_ticks.erase(pid);
        }
    }

    // Update previous snapshots
    m_prev_snapshots = std::move(curr_map);
    m_last_stats = stats;
    return stats;
}

GuardianStats ProcessGuardian::get_last_stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_stats;
}

std::vector<ThrottleRecord> ProcessGuardian::get_active_throttles() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ThrottleRecord> results;
    results.reserve(m_throttled_pids.size());
    for (const auto& [pid, rec] : m_throttled_pids) {
        results.push_back(rec);
    }
    return results;
}

} // namespace surface_optimizer
