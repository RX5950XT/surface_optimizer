#include "core/tray.hpp"
#include "core/ui_state.hpp"
#include <shellapi.h>
#include <cstring>

namespace surface_optimizer {

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT ID_TOGGLE = 1;
constexpr UINT ID_AUTOSTART = 2;
constexpr UINT TRAY_ID = 1;

NOTIFYICONDATAW g_nid{};
SharedUiState* g_ui = nullptr;
bool g_on = true;
bool g_autostart = true;
HICON g_icon_on = nullptr;
HICON g_icon_off = nullptr;
UINT g_taskbar_created = 0;

HICON make_icon(bool on) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = -16;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) {
        return nullptr;
    }

    auto* px = static_cast<unsigned char*>(bits);
    const unsigned char r = on ? 34 : 107;
    const unsigned char g = on ? 197 : 114;
    const unsigned char b = on ? 94 : 128;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int dx = x - 7;
            const int dy = y - 7;
            const bool fill = (dx * dx + dy * dy) <= 36;
            unsigned char* p = px + (y * 16 + x) * 4;
            if (fill) {
                p[0] = b;
                p[1] = g;
                p[2] = r;
                p[3] = 255;
            } else {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
        }
    }

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = CreateBitmap(16, 16, 1, 1, nullptr);
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    DeleteObject(color);
    return icon;
}

void fill_tip() {
    const wchar_t* opt = g_on ? L"開啟" : L"關閉";
    const wchar_t* boot = g_autostart ? L"是" : L"否";
    wchar_t tip[128];
    wsprintfW(tip, L"Surface Optimizer\n優化：%s\n開機自啟動：%s", opt, boot);
    wcsncpy(g_nid.szTip, tip, 127);
    g_nid.szTip[127] = 0;
}

void post_cmd(uint32_t cmd, uint32_t value) {
    if (!g_ui) {
        return;
    }
    g_ui->cmd = cmd;
    g_ui->cmd_value = value;
    g_ui->cmd_seq = g_ui->cmd_seq + 1;
}

void show_menu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_on ? MF_CHECKED : 0), ID_TOGGLE, L"優化");
    AppendMenuW(menu, MF_STRING | (g_autostart ? MF_CHECKED : 0), ID_AUTOSTART, L"開機自啟動");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

} // namespace

bool tray_install(HWND hwnd, SharedUiState* ui) {
    g_ui = ui;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (!g_icon_on) {
        g_icon_on = make_icon(true);
    }
    if (!g_icon_off) {
        g_icon_off = make_icon(false);
    }
    if (ui) {
        g_on = ui->optimizer_on != 0;
        g_autostart = ui->autostart != 0;
    }

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_on ? g_icon_on : g_icon_off;
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    fill_tip();
    return Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

void tray_update(bool optimizer_on, bool autostart) {
    if (g_on == optimizer_on && g_autostart == autostart && g_nid.hWnd) {
        return;
    }
    g_on = optimizer_on;
    g_autostart = autostart;
    if (!g_nid.hWnd) {
        return;
    }
    g_nid.hIcon = g_on ? (g_icon_on ? g_icon_on : g_nid.hIcon)
                       : (g_icon_off ? g_icon_off : g_nid.hIcon);
    fill_tip();
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void tray_remove() {
    if (g_nid.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_nid.hWnd = nullptr;
    }
    if (g_icon_on) {
        DestroyIcon(g_icon_on);
        g_icon_on = nullptr;
    }
    if (g_icon_off) {
        DestroyIcon(g_icon_off);
        g_icon_off = nullptr;
    }
}

HWND tray_create_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SurfaceOptimizerTray";
    RegisterClassExW(&wc);
    return CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);
}

LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbar_created && msg == g_taskbar_created) {
        if (g_nid.hWnd) {
            Shell_NotifyIconW(NIM_ADD, &g_nid);
        }
        return 0;
    }
    if (msg == WM_TRAY) {
        if (lParam == WM_LBUTTONUP) {
            post_cmd(UI_CMD_SET_OPTIMIZER, g_on ? 0u : 1u);
            tray_update(!g_on, g_autostart);
        } else if (lParam == WM_RBUTTONUP) {
            show_menu(hwnd);
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        const UINT id = LOWORD(wParam);
        if (id == ID_TOGGLE) {
            post_cmd(UI_CMD_SET_OPTIMIZER, g_on ? 0u : 1u);
            tray_update(!g_on, g_autostart);
        } else if (id == ID_AUTOSTART) {
            post_cmd(UI_CMD_SET_AUTOSTART, g_autostart ? 0u : 1u);
            tray_update(g_on, !g_autostart);
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        tray_remove();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace surface_optimizer
