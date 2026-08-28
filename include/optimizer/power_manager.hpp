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
    virtual bool is_fast_ramp_active() const;
    virtual uint32_t desired_poll_interval_ms() const;
    virtual void set_last_input_idle_ms(uint32_t idle_ms);
    virtual void set_paused(bool paused);
    virtual bool is_paused() const;

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
    static const GUID GUID_CPMINCORES;
    static const GUID GUID_SMTUNPARKPOLICY;
    static const GUID GUID_SUBGROUP_PCIEXPRESS;
    static const GUID GUID_PCIEXPRESS_ASPM;
    static const GUID GUID_ACDC_SOURCE;
    static const GUID GUID_BATTERY_PERCENT;

    bool refresh_active_scheme_guid();
    bool write_index(const GUID& subgroup, const GUID& setting, bool ac, uint32_t value);
    bool write_index(const GUID& setting, bool ac, uint32_t value);
    bool write_ac_epp(uint32_t epp_value);
    bool write_dc_epp(uint32_t epp_value);
    bool write_ac_boost(uint32_t boost_mode);
    bool write_dc_boost(uint32_t boost_mode);
    bool apply_active_scheme();
    void evaluate_and_apply_governor(bool force_apply = false);
    void restore_baseline();
    void sample_and_update_burst_hold();
    bool sample_process(uint32_t pid, uint64_t& cpu_100ns, uint64_t& io_ops) const;

    mutable std::mutex m_mutex;
    GUID m_active_scheme_guid{};
    bool m_has_active_scheme = false;

    // Saved baseline state for clean restoration on shutdown
    uint32_t m_saved_ac_epp = 60;
    uint32_t m_saved_dc_epp = 80;
    uint32_t m_saved_ac_boost = 2;
    uint32_t m_saved_dc_boost = 0;
    uint32_t m_saved_ac_min = 100;
    uint32_t m_saved_dc_min = 30;
    uint32_t m_saved_ac_cpmin = 100;
    uint32_t m_saved_dc_cpmin = 12;
    uint32_t m_saved_ac_smt = 0;
    uint32_t m_saved_dc_smt = 0;
    uint32_t m_saved_ac_aspm = 2;
    uint32_t m_saved_dc_aspm = 2;
    bool m_saved_baseline = false;
    bool m_paused = false;

    // Active runtime state
    PowerSource m_current_power_source = PowerSource::AC;
    uint32_t m_current_battery_percent = 100;
    PerformanceProfile m_current_profile = PerformanceProfile::Balanced;
    uint32_t m_active_epp = 60;
    uint32_t m_active_boost = 2;
    uint32_t m_active_min = 100;
    uint32_t m_active_unpark = 100;

    std::chrono::steady_clock::time_point m_last_foreground_switch_time{};
    std::chrono::steady_clock::time_point m_quiet_since{};
    bool m_is_fast_ramp_active = false;
    bool m_quiet_timing = false;
    uint32_t m_last_foreground_pid = 0;

    uint64_t m_prev_cpu_100ns = 0;
    uint64_t m_prev_io_ops = 0;
    std::chrono::steady_clock::time_point m_prev_sample_time{};
    bool m_have_process_sample = false;

    uint64_t m_prev_sys_idle = 0;
    uint64_t m_prev_sys_kernel = 0;
    uint64_t m_prev_sys_user = 0;
    bool m_have_system_sample = false;

    uint32_t m_last_input_idle_ms = UINT32_MAX;
    bool m_last_input_from_session = false;
};

} // namespace surface_optimizer
