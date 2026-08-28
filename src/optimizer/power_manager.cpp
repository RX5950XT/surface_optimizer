#include "optimizer/power_manager.hpp"
#include "core/logger.hpp"
#include "core/config.hpp"
#include "core/utils.hpp"
#include <algorithm>

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
const GUID PowerManager::GUID_ACDC_SOURCE = 
    { 0x5d3e4a2d, 0xe6da, 0x4704, { 0x88, 0x6f, 0x38, 0x50, 0x33, 0x8d, 0xa2, 0x1f } };
const GUID PowerManager::GUID_BATTERY_PERCENT = 
    { 0xa7ad8041, 0xb45a, 0x4cae, { 0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1 } };

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

bool PowerManager::initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!refresh_active_scheme_guid()) {
        LOG_ERROR(L"PowerManager: Failed to query active power scheme GUID.");
        return false;
    }

    // Save baseline AC/DC EPP and Boost values for restoration on shutdown
    DWORD ac_epp = 60, dc_epp = 80, ac_boost = 2, dc_boost = 0;
    if (PowerReadACValueIndex(nullptr, &m_active_scheme_guid, &GUID_SUBGROUP_PROCESSOR, &GUID_EPP_POLICY, &ac_epp) == ERROR_SUCCESS) {
        m_saved_ac_epp = ac_epp;
    }
    if (PowerReadDCValueIndex(nullptr, &m_active_scheme_guid, &GUID_SUBGROUP_PROCESSOR, &GUID_EPP_POLICY, &dc_epp) == ERROR_SUCCESS) {
        m_saved_dc_epp = dc_epp;
    }
    if (PowerReadACValueIndex(nullptr, &m_active_scheme_guid, &GUID_SUBGROUP_PROCESSOR, &GUID_PERF_BOOST, &ac_boost) == ERROR_SUCCESS) {
        m_saved_ac_boost = ac_boost;
    }
    if (PowerReadDCValueIndex(nullptr, &m_active_scheme_guid, &GUID_SUBGROUP_PROCESSOR, &GUID_PERF_BOOST, &dc_boost) == ERROR_SUCCESS) {
        m_saved_dc_boost = dc_boost;
    }
    m_saved_baseline = true;

    // Detect initial power source & battery level
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

    LOG_INFO(L"PowerManager initialized. Initial Power Source: " +
             std::wstring(m_current_power_source == PowerSource::AC ? L"AC (Plugged in)" : L"DC (Battery)") +
             L" (" + std::to_wstring(m_current_battery_percent) + L"%)");

    evaluate_and_apply_governor(true);
    return true;
}

bool PowerManager::write_ac_epp(uint32_t epp_value) {
    if (!m_has_active_scheme) return false;
    epp_value = std::min<uint32_t>(epp_value, 100);
    DWORD res = PowerWriteACValueIndex(nullptr, &m_active_scheme_guid, &GUID_SUBGROUP_PROCESSOR, &GUID_EPP_POLICY, epp_value);
    return (res == ERROR_SUCCESS);
}

bool PowerManager::write_dc_epp(uint32_t epp_value) {
    if (!m_has_active_scheme) return false;
    epp_value = std::min<uint32_t>(epp_value, 100);
    DWORD res = PowerWriteDCValueIndex(nullptr, &m_active_scheme_guid, &GUID_SUBGROUP_PROCESSOR, &GUID_EPP_POLICY, epp_value);
    return (res == ERROR_SUCCESS);
}

bool PowerManager::write_ac_boost(uint32_t boost_mode) {
    if (!m_has_active_scheme) return false;
    if (boost_mode > 2) boost_mode = 2;
    DWORD res = PowerWriteACValueIndex(nullptr, &m_active_scheme_guid, &GUID_SUBGROUP_PROCESSOR, &GUID_PERF_BOOST, boost_mode);
    return (res == ERROR_SUCCESS);
}

bool PowerManager::write_dc_boost(uint32_t boost_mode) {
    if (!m_has_active_scheme) return false;
    if (boost_mode > 2) boost_mode = 2;
    DWORD res = PowerWriteDCValueIndex(nullptr, &m_active_scheme_guid, &GUID_SUBGROUP_PROCESSOR, &GUID_PERF_BOOST, boost_mode);
    return (res == ERROR_SUCCESS);
}

bool PowerManager::apply_active_scheme() {
    if (!m_has_active_scheme) return false;
    DWORD res = PowerSetActiveScheme(nullptr, &m_active_scheme_guid);
    return (res == ERROR_SUCCESS);
}

void PowerManager::evaluate_and_apply_governor(bool force_apply) {
    if (!m_has_active_scheme && !refresh_active_scheme_guid()) return;
    auto& config = Config::get_instance();

    PerformanceProfile target_profile = PerformanceProfile::Balanced;
    uint32_t target_epp = 60;
    uint32_t target_boost = 2;

    if (m_current_power_source == PowerSource::AC) {
        if (m_is_fast_ramp_active) {
            target_profile = PerformanceProfile::InstantBoost;
            target_epp = config.power.ac_epp_active; // 0 (Max Performance)
            target_boost = config.power.ac_boost_mode; // 2 (Aggressive)
        } else {
            target_profile = PerformanceProfile::HighPerformance;
            target_epp = config.power.ac_epp_idle; // 60
            target_boost = config.power.ac_boost_mode; // 2
        }
        write_ac_epp(target_epp);
        write_ac_boost(target_boost);
    } else { // Battery (DC) or Unknown
        if (m_current_battery_percent <= config.power.battery_saver_threshold_percent) {
            // Low Battery (< 20%)
            target_profile = PerformanceProfile::BatterySaver;
            target_epp = config.power.battery_saver_epp; // 100 (Max Efficiency)
            target_boost = 0; // Disabled
        } else if (m_is_fast_ramp_active) {
            target_profile = PerformanceProfile::InstantBoost;
            target_epp = config.power.dc_epp_active; // 50
            target_boost = 2; // Aggressive on active interaction
        } else {
            target_profile = PerformanceProfile::Balanced;
            target_epp = config.power.dc_epp_idle; // 80
            target_boost = config.power.dc_boost_mode; // 0 (Disabled to eliminate fan noise and heat soak)
        }
        write_dc_epp(target_epp);
        write_dc_boost(target_boost);
    }

    if (force_apply || target_epp != m_active_epp || target_boost != m_active_boost || target_profile != m_current_profile) {
        apply_active_scheme();
        m_active_epp = target_epp;
        m_active_boost = target_boost;
        m_current_profile = target_profile;
        LOG_INFO(L"Power Governor applied: EPP=" + std::to_wstring(target_epp) +
                 L"%, Boost=" + std::to_wstring(target_boost) +
                 L", Profile=" + std::to_wstring(static_cast<int>(target_profile)));
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

    LOG_DEBUG(L"PowerManager: Foreground switch to PID=" + std::to_wstring(pid) + L" (" + image_name + L"), initiating <100ms fast-ramp...");
    evaluate_and_apply_governor(true);
}

void PowerManager::on_housekeeping() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_is_fast_ramp_active) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_foreground_switch_time).count();
        auto& config = Config::get_instance();

        if (elapsed_ms >= config.power.fast_ramp_duration_ms) {
            m_is_fast_ramp_active = false;
            LOG_DEBUG(L"PowerManager: Fast-ramp duration expired (" + std::to_wstring(elapsed_ms) + L"ms), reverting to idle power profile.");
            evaluate_and_apply_governor(false);
        }
    }
}

bool PowerManager::set_epp(uint32_t epp_value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_has_active_scheme && !refresh_active_scheme_guid()) return false;
    epp_value = std::min<uint32_t>(epp_value, 100);

    bool ok = false;
    if (m_current_power_source == PowerSource::AC) {
        ok = write_ac_epp(epp_value);
    } else {
        ok = write_dc_epp(epp_value);
    }

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

    bool ok = false;
    if (m_current_power_source == PowerSource::AC) {
        ok = write_ac_boost(mode);
    } else {
        ok = write_dc_boost(mode);
    }

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
        LOG_INFO(L"PowerManager: Restoring original power baseline (AC_EPP=" + std::to_wstring(m_saved_ac_epp) +
                 L", DC_EPP=" + std::to_wstring(m_saved_dc_epp) +
                 L", AC_Boost=" + std::to_wstring(m_saved_ac_boost) +
                 L", DC_Boost=" + std::to_wstring(m_saved_dc_boost) + L")...");
        write_ac_epp(m_saved_ac_epp);
        write_dc_epp(m_saved_dc_epp);
        write_ac_boost(m_saved_ac_boost);
        write_dc_boost(m_saved_dc_boost);
        apply_active_scheme();
    }
    m_has_active_scheme = false;
}

} // namespace surface_optimizer
