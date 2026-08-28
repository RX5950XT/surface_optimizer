#pragma once

#include <windows.h>

namespace surface_optimizer {

struct SharedUiState;

bool tray_install(HWND hwnd, SharedUiState* ui);
void tray_update(bool optimizer_on, bool autostart);
void tray_remove();
LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
HWND tray_create_window();

} // namespace surface_optimizer
