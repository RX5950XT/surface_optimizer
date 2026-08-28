#pragma once

#include <cstdint>

namespace surface_optimizer {

inline bool is_memory_growth_suspicious(int64_t working_set_delta_bytes) {
    constexpr int64_t threshold_bytes = 100LL * 1024 * 1024;
    return working_set_delta_bytes > threshold_bytes;
}

} // namespace surface_optimizer
