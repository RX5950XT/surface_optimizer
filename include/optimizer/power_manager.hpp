#pragma once

#include <windows.h>
#include <powrprof.h>
#include <string>
#include <cstdint>
#include <chrono>
#include <mutex>

namespace surface_optimizer {

enum class PowerSource {
    AC,
    Battery,
    Unknown
};

enum class PerformanceProfile {
    BatterySaver,
    Balanced,
    HighPerformance,
    InstantBoost
};

class PowerManager {
public:
    static PowerManager& get_instance();

    virtual bool initialize();
    virtual void on_power_source_changed(PowerSource source);
    virtual void on_battery_percent_changed(uint32_t percent);
    virtual void on_foreground_process_changed(uint32_t pid, const std::wstring& image_name);
    virtual void on_housekeeping();

    virtual bool set_epp(uint32_t epp_value); // 0 (Max Perf) ~ 100 (Max Energy Save)
    virtual bool set_boost_mode(uint32_t mode); // 0=Disabled, 1=Enabled, 2=Aggressive
    virtual uint32_t get_current_epp() const;
    virtual uint32_t get_current_boost_mode() const;
    virtual PowerSource get_current_power_source() const;
    virtual uint32_t get_current_battery_percent() const;
    virtual PerformanceProfile get_current_profile() const;
    virtual std::wstring get_active_scheme_name() const;

    virtual void shutdown();

    virtual ~PowerManager() = default;

private:
    PowerManager();
    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    // Power Setting GUID constants
    static const GUID GUID_SUBGROUP_PROCESSOR;
    static const GUID GUID_EPP_POLICY;
    static const GUID GUID_PERF_BOOST;
    static const GUID GUID_PROCTHROTTLE_MIN;
    static const GUID GUID_PROCTHROTTLE_MAX;
    static const GUID GUID_ACDC_SOURCE;
    static const GUID GUID_BATTERY_PERCENT;

    // Internal Win32 scheme & value helpers
    bool refresh_active_scheme_guid();
    bool write_ac_epp(uint32_t epp_value);
    bool write_dc_epp(uint32_t epp_value);
    bool write_ac_boost(uint32_t boost_mode);
    bool write_dc_boost(uint32_t boost_mode);
    bool apply_active_scheme();
    void evaluate_and_apply_governor(bool force_apply = false);

    mutable std::mutex m_mutex;
    GUID m_active_scheme_guid{};
    bool m_has_active_scheme = false;

    // Saved baseline state for clean restoration on shutdown
    uint32_t m_saved_ac_epp = 60;
    uint32_t m_saved_dc_epp = 80;
    uint32_t m_saved_ac_boost = 2;
    uint32_t m_saved_dc_boost = 0;
    bool m_saved_baseline = false;

    // Active runtime state
    PowerSource m_current_power_source = PowerSource::AC;
    uint32_t m_current_battery_percent = 100;
    PerformanceProfile m_current_profile = PerformanceProfile::Balanced;
    uint32_t m_active_epp = 60;
    uint32_t m_active_boost = 2;

    // Fast-ramp responsiveness tracking
    std::chrono::steady_clock::time_point m_last_foreground_switch_time{};
    bool m_is_fast_ramp_active = false;
    uint32_t m_last_foreground_pid = 0;
};

} // namespace surface_optimizer
