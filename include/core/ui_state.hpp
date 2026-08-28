#pragma once

#include <cstdint>

namespace surface_optimizer {

inline constexpr const wchar_t* UI_MAP_NAME = L"Global\\SurfaceOptimizerUiState";

struct SharedUiState {
    uint32_t optimizer_on = 1;
    uint32_t autostart = 1;
    uint32_t stop_watch = 0;
    uint32_t cmd = 0;       // 0=none, 1=set optimizer_on, 2=set autostart
    uint32_t cmd_value = 0;
    uint32_t cmd_seq = 0;
    uint32_t ack_seq = 0;
};

inline constexpr uint32_t UI_CMD_SET_OPTIMIZER = 1;
inline constexpr uint32_t UI_CMD_SET_AUTOSTART = 2;

} // namespace surface_optimizer
