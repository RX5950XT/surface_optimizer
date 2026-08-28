#pragma once

#include <cstdint>

namespace surface_optimizer {

// Pure burst hold/drop policy. No Win32 here so it can be unit-tested.
struct BurstHoldInput {
    bool sampled = false;          // process times were readable
    bool cpu_busy = false;
    bool io_busy = false;
    bool gpu_busy = false;
    bool system_busy = false;      // only used when pid is unknown (boot)
    bool recent_input = false;
    bool use_system_busy = false;
    uint32_t elapsed_ms = 0;
    uint32_t grace_ms = 200;
    uint32_t fallback_hold_ms = 3000;
};

inline double cpu_percent_one_core(uint64_t time_delta_100ns, uint64_t elapsed_100ns) {
    if (elapsed_100ns == 0) {
        return 0.0;
    }
    return (100.0 * static_cast<double>(time_delta_100ns)) / static_cast<double>(elapsed_100ns);
}

// GetSystemTimes: kernel time includes idle time.
inline double system_busy_percent(uint64_t idle_delta_100ns,
                                  uint64_t kernel_delta_100ns,
                                  uint64_t user_delta_100ns) {
    const uint64_t total = kernel_delta_100ns + user_delta_100ns;
    if (total == 0) {
        return 0.0;
    }
    uint64_t busy = user_delta_100ns;
    if (kernel_delta_100ns >= idle_delta_100ns) {
        busy += (kernel_delta_100ns - idle_delta_100ns);
    }
    return (100.0 * static_cast<double>(busy)) / static_cast<double>(total);
}

inline bool should_hold_boost(const BurstHoldInput& in) {
    if (in.elapsed_ms < in.grace_ms) {
        return true;
    }
    if (in.recent_input) {
        return true;
    }
    if (in.gpu_busy) {
        return true;
    }
    if (in.sampled) {
        return in.cpu_busy || in.io_busy;
    }
    if (in.use_system_busy && in.system_busy) {
        return true;
    }
    return in.elapsed_ms < in.fallback_hold_ms;
}

} // namespace surface_optimizer
