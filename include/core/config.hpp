#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace surface_optimizer {

struct PowerConfig {
    uint32_t ac_epp_idle = 60;
    uint32_t ac_epp_active = 0;
    uint32_t dc_epp_idle = 80;
    uint32_t dc_epp_active = 50;
    uint32_t battery_saver_epp = 100;
    uint32_t battery_saver_threshold_percent = 20;
    uint32_t ac_boost_mode = 2; // 0=Disabled, 2=Aggressive
    uint32_t dc_boost_mode = 0; // 0=Disabled, 2=Aggressive
    uint32_t fast_ramp_duration_ms = 3000;
};

struct MemoryConfig {
    uint32_t pressure_threshold_percent = 75;
    uint32_t trim_interval_seconds = 60;
    bool enable_standby_purge = true;
    bool trim_idle_processes_on_ac = true;
    bool trim_idle_processes_on_dc = true;
};

struct ProcessGovernorConfig {
    double cpu_hog_threshold_percent = 15.0;
    uint32_t cpu_hog_sustain_seconds = 30;
    bool enable_eco_qos = true;
    bool enable_priority_demotion = true;
    std::vector<std::wstring> allowlist = {
        L"smss.exe", L"csrss.exe", L"wininit.exe", L"services.exe",
        L"lsass.exe", L"svchost.exe", L"fontdrvhost.exe", L"dwm.exe",
        L"explorer.exe", L"sihost.exe", L"taskhostw.exe", L"audiodg.exe",
        L"MsMpEng.exe", L"SecurityHealthService.exe", L"surface_optimizer.exe"
    };
};

struct DaemonConfig {
    uint32_t housekeeping_interval_ac_ms = 5000;
    uint32_t housekeeping_interval_dc_ms = 15000;
    uint32_t housekeeping_interval_idle_ms = 60000;
    std::wstring log_level = L"INFO";
    std::wstring log_file_path = L"C:\\ProgramData\\surface_optimizer\\surface_optimizer.log";
    bool log_to_console = true;
    bool log_to_file = true;
};

class Config {
public:
    static Config& get_instance();

    bool load_from_file(const std::wstring& file_path);
    bool save_to_file(const std::wstring& file_path) const;
    void reset_to_defaults();

    PowerConfig power;
    MemoryConfig memory;
    ProcessGovernorConfig governor;
    DaemonConfig daemon;

private:
    Config();
};

} // namespace surface_optimizer