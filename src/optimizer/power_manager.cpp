#include "optimizer/power_manager.hpp"
#include "optimizer/burst_policy.hpp"
#include "core/logger.hpp"
#include "core/config.hpp"
#include "core/utils.hpp"
#include <algorithm>
#include <pdh.h>
#include <vector>
#include <cwctype>

namespace surface_optimizer {

const GUID PowerManager::GUID_SUBGROUP_PROCESSOR =
    { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
const GUID PowerManager::GUID_EPP_POLICY =
    { 0x36687f9e, 0xe3a5, 0x4dbf, { 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63 } };
const GUID PowerManager::GUID_PERF_BOOST =
    { 0xbe337238, 0x0d82, 0x4146, { 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7 } };
const GUID PowerManager::GUID_PROCTHROTTLE_MIN =
    { 0x893dee8e, 0x2bef, 0x41e0, { 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c } };
const GUID PowerManager::GUID_PROCTHROTTLE_MAX =
    { 0xbc5038f7, 0x23e0, 0x4960, { 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec } };
const GUID PowerManager::GUID_CPMINCORES =
    { 0x0cc5b647, 0xc1df, 0x4637, { 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83 } };
const GUID PowerManager::GUID_SMTUNPARKPOLICY =
    { 0xb28a6829, 0xc5f7, 0x444e, { 0x8f, 0x61, 0x10, 0xe2, 0x4e, 0x85, 0xc5, 0x32 } };
const GUID PowerManager::GUID_SUBGROUP_PCIEXPRESS =
    { 0x501a4d13, 0x42af, 0x4429, { 0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20 } };
const GUID PowerManager::GUID_PCIEXPRESS_ASPM =
    { 0xee12f906, 0xd277, 0x404b, { 0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5 } };
const GUID PowerManager::GUID_ACDC_SOURCE =
    { 0x5d3e4a2d, 0xe6da, 0x4704, { 0x88, 0x6f, 0x38, 0x50, 0x33, 0x8d, 0xa2, 0x1f } };
const GUID PowerManager::GUID_BATTERY_PERCENT =
    { 0xa7ad8041, 0xb45a, 0x4cae, { 0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1 } };

namespace {

uint64_t filetime_to_100ns(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

uint32_t query_last_input_idle_ms() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) {
        return UINT32_MAX;
    }
    return GetTickCount() - info.dwTime;
}

bool name_has_idle_engine(const wchar_t* name) {
    if (!name) {
        return true;
    }
    const wchar_t* needle = L"engtype_idle";
    for (const wchar_t* s = name; *s; ++s) {
        size_t i = 0;
        while (needle[i] && s[i] &&
               towlower(static_cast<wint_t>(s[i])) == needle[i]) {
            ++i;
        }
        if (needle[i] == 0) {
            return true;
        }
    }
    return false;
}

struct GpuBusySampler {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool tried = false;
    bool ready = false;
    bool baselined = false;

    void close() {
        if (query) {
            PdhCloseQuery(query);
            query = nullptr;
        }
        counter = nullptr;
        tried = false;
        ready = false;
        baselined = false;
    }

    void ensure() {
        if (tried) {
            return;
        }
        tried = true;
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            LOG_WARN(L"GPU sampler: PdhOpenQueryW failed.");
            return;
        }
        PDH_STATUS add = PdhAddEnglishCounterW(
            query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
        if (add != ERROR_SUCCESS) {
            LOG_WARN(L"GPU sampler: GPU Engine counter unavailable.");
            PdhCloseQuery(query);
            query = nullptr;
            counter = nullptr;
            return;
        }
        ready = true;
        LOG_INFO(L"GPU sampler: PDH GPU Engine counter armed.");
    }

    bool is_busy(double threshold_percent) {
        ensure();
        if (!ready) {
            return false;
        }
        PDH_STATUS st = PdhCollectQueryData(query);
        if (!baselined) {
            baselined = true;
            return false;
        }
        if (st != ERROR_SUCCESS) {
            return false;
        }
        DWORD buf_size = 0;
        DWORD item_count = 0;
        st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &buf_size, &item_count, nullptr);
        if (buf_size == 0 || item_count == 0) {
            return false;
        }
        std::vector<uint8_t> buf(buf_size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &buf_size, &item_count, items);
        if (st != ERROR_SUCCESS) {
            return false;
        }
        double peak = 0.0;
        for (DWORD i = 0; i < item_count; ++i) {
            if (name_has_idle_engine(items[i].szName)) {
                continue;
            }
            if (items[i].FmtValue.CStatus != 0) {
                continue;
            }
            if (items[i].FmtValue.doubleValue > peak) {
                peak = items[i].FmtValue.doubleValue;
            }
        }
        return peak >= threshold_percent;
    }
};

GpuBusySampler g_gpu;

} // namespace

PowerManager& PowerManager::get_instance() {
    static PowerManager instance;
    return instance;
}

PowerManager::PowerManager() = default;

bool PowerManager::refresh_active_scheme_guid() {
    GUID* pGuid = nullptr;
    DWORD res = PowerGetActiveScheme(nullptr, &pGuid);
    if (res == ERROR_SUCCESS && pGuid != nullptr) {
        m_active_scheme_guid = *pGuid;
        m_has_active_scheme = true;
        LocalFree(pGuid);
        return true;
    }
    m_has_active_scheme = false;
    return false;
}

bool PowerManager::write_index(const GUID& subgroup, const GUID& setting, bool ac, uint32_t value) {
    if (!m_has_active_scheme) return false;
    DWORD res = ac
        ? PowerWriteACValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&subgroup), const_cast<GUID*>(&setting), value)
        : PowerWriteDCValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&subgroup), const_cast<GUID*>(&setting), value);
    return res == ERROR_SUCCESS;
}

bool PowerManager::write_index(const GUID& setting, bool ac, uint32_t value) {
    return write_index(GUID_SUBGROUP_PROCESSOR, setting, ac, value);
}

bool PowerManager::initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!refresh_active_scheme_guid()) {
        LOG_ERROR(L"PowerManager: Failed to query active power scheme GUID.");
        return false;
    }

    DWORD ac_epp = 60, dc_epp = 80, ac_boost = 2, dc_boost = 0;
    DWORD ac_min = 100, dc_min = 30, ac_cpmin = 100, dc_cpmin = 12;
    DWORD ac_smt = 0, dc_smt = 0, ac_aspm = 2, dc_aspm = 2;
    PowerReadACValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_EPP_POLICY), &ac_epp);
    PowerReadDCValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_EPP_POLICY), &dc_epp);
    PowerReadACValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_PERF_BOOST), &ac_boost);
    PowerReadDCValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_PERF_BOOST), &dc_boost);
    PowerReadACValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_PROCTHROTTLE_MIN), &ac_min);
    PowerReadDCValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_PROCTHROTTLE_MIN), &dc_min);
    PowerReadACValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_CPMINCORES), &ac_cpmin);
    PowerReadDCValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_CPMINCORES), &dc_cpmin);
    PowerReadACValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_SMTUNPARKPOLICY), &ac_smt);
    PowerReadDCValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PROCESSOR), const_cast<GUID*>(&GUID_SMTUNPARKPOLICY), &dc_smt);
    PowerReadACValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PCIEXPRESS), const_cast<GUID*>(&GUID_PCIEXPRESS_ASPM), &ac_aspm);
    PowerReadDCValueIndex(nullptr, &m_active_scheme_guid, const_cast<GUID*>(&GUID_SUBGROUP_PCIEXPRESS), const_cast<GUID*>(&GUID_PCIEXPRESS_ASPM), &dc_aspm);

    m_saved_ac_epp = ac_epp;
    m_saved_dc_epp = dc_epp;
    m_saved_ac_boost = ac_boost;
    m_saved_dc_boost = dc_boost;
    m_saved_ac_min = ac_min;
    m_saved_dc_min = dc_min;
    m_saved_ac_cpmin = ac_cpmin;
    m_saved_dc_cpmin = dc_cpmin;
    m_saved_ac_smt = ac_smt;
    m_saved_dc_smt = dc_smt;
    m_saved_ac_aspm = ac_aspm;
    m_saved_dc_aspm = dc_aspm;
    m_saved_baseline = true;

    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) {
        if (sps.ACLineStatus == 1) {
            m_current_power_source = PowerSource::AC;
        } else if (sps.ACLineStatus == 0) {
            m_current_power_source = PowerSource::Battery;
        } else {
            m_current_power_source = PowerSource::Unknown;
        }
        if (sps.BatteryLifePercent != 255) {
            m_current_battery_percent = sps.BatteryLifePercent;
        }
    }

    // One LP per physical core on first unpark (Win11 desktop default). Homogeneous 4C/8T.
    write_index(GUID_SMTUNPARKPOLICY, true, 1);
    write_index(GUID_SMTUNPARKPOLICY, false, 1);
    // High Performance ships ASPM=Max on AC; that parks the NVMe link and stalls first I/O.
    write_index(GUID_SUBGROUP_PCIEXPRESS, GUID_PCIEXPRESS_ASPM, true, 0);
    write_index(GUID_SUBGROUP_PCIEXPRESS, GUID_PCIEXPRESS_ASPM, false, 2);

    m_last_foreground_switch_time = std::chrono::steady_clock::now();
    m_is_fast_ramp_active = true;
    m_quiet_timing = false;

    LOG_INFO(L"PowerManager initialized. Initial Power Source: " +
             std::wstring(m_current_power_source == PowerSource::AC ? L"AC (Plugged in)" : L"DC (Battery)") +
             L" (" + std::to_wstring(m_current_battery_percent) + L"%)");

    evaluate_and_apply_governor(true);
    return true;
}

bool PowerManager::write_ac_epp(uint32_t epp_value) {
    epp_value = std::min<uint32_t>(epp_value, 100);
    return write_index(GUID_EPP_POLICY, true, epp_value);
}

bool PowerManager::write_dc_epp(uint32_t epp_value) {
    epp_value = std::min<uint32_t>(epp_value, 100);
    return write_index(GUID_EPP_POLICY, false, epp_value);
}

bool PowerManager::write_ac_boost(uint32_t boost_mode) {
    if (boost_mode > 2) boost_mode = 2;
    return write_index(GUID_PERF_BOOST, true, boost_mode);
}

bool PowerManager::write_dc_boost(uint32_t boost_mode) {
    if (boost_mode > 2) boost_mode = 2;
    return write_index(GUID_PERF_BOOST, false, boost_mode);
}

bool PowerManager::apply_active_scheme() {
    if (!m_has_active_scheme) return false;
    return PowerSetActiveScheme(nullptr, &m_active_scheme_guid) == ERROR_SUCCESS;
}

void PowerManager::evaluate_and_apply_governor(bool force_apply) {
    if (!m_has_active_scheme && !refresh_active_scheme_guid()) return;
    auto& config = Config::get_instance();

    PerformanceProfile target_profile = PerformanceProfile::Balanced;
    uint32_t target_epp = 60;
    uint32_t target_boost = 2;
    uint32_t target_min = 5;
    uint32_t target_unpark = 25;
    const bool ac = (m_current_power_source == PowerSource::AC);

    if (m_current_battery_percent <= config.power.battery_saver_threshold_percent && !ac) {
        target_profile = PerformanceProfile::BatterySaver;
        target_epp = config.power.battery_saver_epp;
        target_boost = 0;
        target_min = 5;
        target_unpark = 12;
    } else if (m_is_fast_ramp_active) {
        target_profile = PerformanceProfile::InstantBoost;
        target_epp = ac ? config.power.ac_epp_active : config.power.dc_epp_active;
        target_boost = ac ? config.power.ac_boost_mode : 2;
        target_min = 100;
        target_unpark = 100;
    } else if (ac) {
        target_profile = PerformanceProfile::HighPerformance;
        target_epp = config.power.ac_epp_idle;
        target_boost = config.power.ac_boost_mode;
        target_min = 5;
        target_unpark = 25;
    } else {
        target_profile = PerformanceProfile::Balanced;
        target_epp = config.power.dc_epp_idle;
        target_boost = config.power.dc_boost_mode;
        target_min = 5;
        target_unpark = 12;
    }

    if (ac) {
        write_ac_epp(target_epp);
        write_ac_boost(target_boost);
        write_index(GUID_PROCTHROTTLE_MIN, true, target_min);
        write_index(GUID_CPMINCORES, true, target_unpark);
    } else {
        write_dc_epp(target_epp);
        write_dc_boost(target_boost);
        write_index(GUID_PROCTHROTTLE_MIN, false, target_min);
        write_index(GUID_CPMINCORES, false, target_unpark);
    }

    if (force_apply || target_epp != m_active_epp || target_boost != m_active_boost ||
        target_profile != m_current_profile || target_min != m_active_min ||
        target_unpark != m_active_unpark) {
        apply_active_scheme();
        m_active_epp = target_epp;
        m_active_boost = target_boost;
        m_current_profile = target_profile;
        m_active_min = target_min;
        m_active_unpark = target_unpark;
        LOG_INFO(L"Power Governor applied: EPP=" + std::to_wstring(target_epp) +
                 L"%, Boost=" + std::to_wstring(target_boost) +
                 L", Min=" + std::to_wstring(target_min) +
                 L"%, Unpark=" + std::to_wstring(target_unpark) +
                 L"%, Profile=" + std::to_wstring(static_cast<int>(target_profile)));
    }
}

void PowerManager::on_power_source_changed(PowerSource source) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current_power_source != source) {
        m_current_power_source = source;
        LOG_INFO(L"PowerManager: Power source changed to " +
                 std::wstring(source == PowerSource::AC ? L"AC" : L"Battery"));
        evaluate_and_apply_governor(true);
    }
}

void PowerManager::on_battery_percent_changed(uint32_t percent) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t old_percent = m_current_battery_percent;
    m_current_battery_percent = percent;

    auto& config = Config::get_instance();
    bool old_low = (old_percent <= config.power.battery_saver_threshold_percent);
    bool new_low = (percent <= config.power.battery_saver_threshold_percent);

    if (old_low != new_low) {
        LOG_INFO(L"PowerManager: Battery threshold state changed (Battery: " + std::to_wstring(percent) + L"%)");
        evaluate_and_apply_governor(true);
    }
}

void PowerManager::on_foreground_process_changed(uint32_t pid, const std::wstring& image_name) {
    if (pid == 0 || pid == m_last_foreground_pid) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_foreground_pid = pid;
    m_last_foreground_switch_time = std::chrono::steady_clock::now();
    m_is_fast_ramp_active = true;
    m_quiet_timing = false;
    m_have_process_sample = false;

    LOG_DEBUG(L"PowerManager: Foreground switch to PID=" + std::to_wstring(pid) + L" (" + image_name + L"), fast-ramp");
    evaluate_and_apply_governor(true);
}

bool PowerManager::sample_process(uint32_t pid, uint64_t& cpu_100ns, uint64_t& io_ops) const {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return false;
    }
    FILETIME create{}, exit{}, kernel{}, user{};
    bool ok = GetProcessTimes(h, &create, &exit, &kernel, &user) != 0;
    IO_COUNTERS io{};
    if (ok) {
        cpu_100ns = filetime_to_100ns(kernel) + filetime_to_100ns(user);
        if (GetProcessIoCounters(h, &io)) {
            io_ops = io.ReadOperationCount + io.WriteOperationCount + io.OtherOperationCount;
        } else {
            io_ops = 0;
        }
    }
    CloseHandle(h);
    return ok;
}

void PowerManager::sample_and_update_burst_hold() {
    auto& config = Config::get_instance();
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_foreground_switch_time).count());

    BurstHoldInput in{};
    in.grace_ms = config.power.boost_grace_ms;
    in.fallback_hold_ms = config.power.fast_ramp_duration_ms;
    in.elapsed_ms = elapsed_ms;

    uint32_t idle_ms = m_last_input_from_session ? m_last_input_idle_ms : query_last_input_idle_ms();
    in.recent_input = (idle_ms != UINT32_MAX && idle_ms <= config.power.last_input_hold_ms);
    in.gpu_busy = g_gpu.is_busy(config.power.busy_gpu_percent_threshold);

    if (m_last_foreground_pid != 0) {
        uint64_t cpu = 0, io = 0;
        if (sample_process(m_last_foreground_pid, cpu, io)) {
            in.sampled = true;
            if (m_have_process_sample) {
                auto sample_elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_prev_sample_time).count() / 100;
                if (sample_elapsed < 1) sample_elapsed = 1;
                double pct = cpu_percent_one_core(cpu - m_prev_cpu_100ns, static_cast<uint64_t>(sample_elapsed));
                in.cpu_busy = pct >= config.power.busy_cpu_percent_threshold;
                in.io_busy = io > m_prev_io_ops;
            } else {
                in.cpu_busy = true; // first sample: stay boosted until we have a delta
            }
            m_prev_cpu_100ns = cpu;
            m_prev_io_ops = io;
            m_prev_sample_time = now;
            m_have_process_sample = true;
        }
    } else {
        in.use_system_busy = true;
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            uint64_t idle_t = filetime_to_100ns(idle);
            uint64_t kernel_t = filetime_to_100ns(kernel);
            uint64_t user_t = filetime_to_100ns(user);
            if (m_have_system_sample) {
                double pct = system_busy_percent(
                    idle_t - m_prev_sys_idle,
                    kernel_t - m_prev_sys_kernel,
                    user_t - m_prev_sys_user);
                in.system_busy = pct >= 25.0;
            } else {
                in.system_busy = true;
            }
            m_prev_sys_idle = idle_t;
            m_prev_sys_kernel = kernel_t;
            m_prev_sys_user = user_t;
            m_have_system_sample = true;
        }
    }

    bool hold = should_hold_boost(in);
    if (hold) {
        m_quiet_timing = false;
        if (!m_is_fast_ramp_active) {
            m_is_fast_ramp_active = true;
            m_last_foreground_switch_time = now;
        }
    } else {
        if (!m_quiet_timing) {
            m_quiet_timing = true;
            m_quiet_since = now;
        }
        auto quiet_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_quiet_since).count();
        if (quiet_ms >= static_cast<int64_t>(config.power.idle_hysteresis_ms) && m_is_fast_ramp_active) {
            m_is_fast_ramp_active = false;
            LOG_DEBUG(L"PowerManager: burst quiet for " + std::to_wstring(quiet_ms) + L"ms, dropping to idle");
        }
    }
}

void PowerManager::on_housekeeping() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool was_boost = m_is_fast_ramp_active;
    sample_and_update_burst_hold();
    if (was_boost != m_is_fast_ramp_active) {
        evaluate_and_apply_governor(false);
    }
}

void PowerManager::set_last_input_idle_ms(uint32_t idle_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_input_idle_ms = idle_ms;
    m_last_input_from_session = true;
}

bool PowerManager::is_fast_ramp_active() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_is_fast_ramp_active;
}

uint32_t PowerManager::desired_poll_interval_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& config = Config::get_instance();
    if (m_is_fast_ramp_active) {
        return config.power.busy_poll_interval_ms;
    }
    return config.power.idle_poll_interval_ms;
}

bool PowerManager::set_epp(uint32_t epp_value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_has_active_scheme && !refresh_active_scheme_guid()) return false;
    epp_value = std::min<uint32_t>(epp_value, 100);

    bool ok = (m_current_power_source == PowerSource::AC) ? write_ac_epp(epp_value) : write_dc_epp(epp_value);
    if (ok) {
        apply_active_scheme();
        m_active_epp = epp_value;
    }
    return ok;
}

bool PowerManager::set_boost_mode(uint32_t mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_has_active_scheme && !refresh_active_scheme_guid()) return false;
    if (mode > 2) mode = 2;

    bool ok = (m_current_power_source == PowerSource::AC) ? write_ac_boost(mode) : write_dc_boost(mode);
    if (ok) {
        apply_active_scheme();
        m_active_boost = mode;
    }
    return ok;
}

uint32_t PowerManager::get_current_epp() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active_epp;
}

uint32_t PowerManager::get_current_boost_mode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active_boost;
}

PowerSource PowerManager::get_current_power_source() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_current_power_source;
}

uint32_t PowerManager::get_current_battery_percent() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_current_battery_percent;
}

PerformanceProfile PowerManager::get_current_profile() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_current_profile;
}

std::wstring PowerManager::get_active_scheme_name() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_has_active_scheme) return L"Unknown";
    return Utils::guid_to_wstring(m_active_scheme_guid);
}

void PowerManager::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_saved_baseline && m_has_active_scheme) {
        LOG_INFO(L"PowerManager: Restoring original power baseline...");
        write_ac_epp(m_saved_ac_epp);
        write_dc_epp(m_saved_dc_epp);
        write_ac_boost(m_saved_ac_boost);
        write_dc_boost(m_saved_dc_boost);
        write_index(GUID_PROCTHROTTLE_MIN, true, m_saved_ac_min);
        write_index(GUID_PROCTHROTTLE_MIN, false, m_saved_dc_min);
        write_index(GUID_CPMINCORES, true, m_saved_ac_cpmin);
        write_index(GUID_CPMINCORES, false, m_saved_dc_cpmin);
        write_index(GUID_SMTUNPARKPOLICY, true, m_saved_ac_smt);
        write_index(GUID_SMTUNPARKPOLICY, false, m_saved_dc_smt);
        write_index(GUID_SUBGROUP_PCIEXPRESS, GUID_PCIEXPRESS_ASPM, true, m_saved_ac_aspm);
        write_index(GUID_SUBGROUP_PCIEXPRESS, GUID_PCIEXPRESS_ASPM, false, m_saved_dc_aspm);
        apply_active_scheme();
    }
    g_gpu.close();
    m_has_active_scheme = false;
}

} // namespace surface_optimizer
