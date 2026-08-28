#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace surface_optimizer {

struct CpuSetEntry {
    uint32_t id = 0;
    uint8_t logical_index = 0;
    uint8_t core_index = 0;
    uint8_t efficiency_class = 0;
    bool parked = false;
};

struct LogicalMhz {
    uint32_t number = 0;
    uint32_t current_mhz = 0;
    uint32_t max_mhz = 0;
    uint32_t mhz_limit = 0;
};

struct CpuidPower {
    uint32_t eax06 = 0;
    bool digital_temp_sensor = false;
    bool turbo_boost = false;
    bool hwp = false;
    bool hwp_notification = false;
    bool hwp_activity_window = false;
    bool hwp_epp = false;
    bool hwp_package_request = false;
    bool hdc = false;
    bool turbo_boost_max_3 = false;
    bool peci_override = false;
    bool hybrid = false;
    char brand[64] = {};
};

struct PpmValues {
    uint32_t epp_ac = 0;
    uint32_t epp_dc = 0;
    uint32_t boost_ac = 0;
    uint32_t boost_dc = 0;
    uint32_t min_ac = 0;
    uint32_t min_dc = 0;
    uint32_t max_ac = 0;
    uint32_t max_dc = 0;
    uint32_t cpmin_ac = 0;
    uint32_t cpmin_dc = 0;
    uint32_t cpmax_ac = 0;
    uint32_t cpmax_dc = 0;
    uint32_t softpark_ac = 0;
    uint32_t softpark_dc = 0;
    uint32_t smt_unpark_ac = 0;
    uint32_t smt_unpark_dc = 0;
    uint32_t autonomous_ac = 0;
    uint32_t autonomous_dc = 0;
    uint32_t autonomous_window_ac = 0;
    uint32_t autonomous_window_dc = 0;
    uint32_t aspm_ac = 0;
    uint32_t aspm_dc = 0;
    bool read_ok = false;
    std::wstring scheme_name;
};

struct EppApplyLatency {
    bool ok = false;
    double activate_us = 0.0;
    uint32_t from_epp = 0;
    uint32_t to_epp = 0;
};

struct PlatformSnapshot {
    uint32_t cores = 0;
    uint32_t logical = 0;
    CpuidPower cpuid;
    PpmValues ppm;
    std::vector<CpuSetEntry> cpu_sets;
    std::vector<LogicalMhz> mhz;
    uint8_t efficiency_class_min = 0;
    uint8_t efficiency_class_max = 0;
    bool homogeneous = true;
    bool any_parked = false;
    bool rapl_readable = false;
    std::wstring rapl_note;
};

class PlatformProbe {
public:
    static PlatformSnapshot capture();
    static EppApplyLatency measure_epp_apply_latency();
    static std::wstring format_snapshot(const PlatformSnapshot& snap);
};

} // namespace surface_optimizer
