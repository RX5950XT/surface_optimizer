#pragma once

#include <cstdint>

namespace surface_optimizer {

// One physical core for DWM/kernel on 4+ core Ice Lake. Fewer cores: do not reserve.
inline bool should_reserve_os_core(uint32_t physical_cores) {
    return physical_cores >= 4;
}

inline uint64_t user_affinity_mask(uint64_t system_mask, uint64_t os_core_mask) {
    if (system_mask == 0) {
        return 0;
    }
    const uint64_t user = system_mask & ~os_core_mask;
    return user == 0 ? system_mask : user;
}

inline bool should_restrict_off_os_core(bool allowlisted, bool is_self, bool is_system_pid) {
    if (is_system_pid || is_self || allowlisted) {
        return false;
    }
    return true;
}

inline bool affinity_uses_os_core(uint64_t process_mask, uint64_t os_core_mask) {
    return (process_mask & os_core_mask) != 0;
}

} // namespace surface_optimizer
