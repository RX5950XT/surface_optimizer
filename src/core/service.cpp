#include "core/service.hpp"
#include "core/logger.hpp"
#include "core/config.hpp"
#include "core/utils.hpp"
#include "core/ui_state.hpp"
#include "core/tray.hpp"
#include "optimizer/power_manager.hpp"
#include "optimizer/memory_manager.hpp"
#include "optimizer/process_guardian.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <sddl.h>
#include <wtsapi32.h>
#include <cstring>

#ifndef PBT_POWERSETTINGCHANGE
#define PBT_POWERSETTINGCHANGE 0x8013
#endif

#ifndef DEVICE_NOTIFY_SERVICE_HANDLE
#define DEVICE_NOTIFY_SERVICE_HANDLE 0x00000001
#endif

#ifndef SERVICE_CONFIG_FAILURE_ACTIONS
#define SERVICE_CONFIG_FAILURE_ACTIONS 2
#endif

namespace surface_optimizer {

namespace {

static const GUID GUID_ACDC_POWER_SOURCE_VAL = 
    { 0x5d3e4a2d, 0xe6da, 0x4704, { 0x88, 0x6f, 0x38, 0x50, 0x33, 0x8d, 0xa2, 0x1f } };
static const GUID GUID_BATTERY_PERCENTAGE_REMAINING_VAL = 
    { 0xa7ad8041, 0xb45a, 0x4cae, { 0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1 } };

bool create_ipc_security_attributes(SECURITY_ATTRIBUTES& attrs, PSECURITY_DESCRIPTOR& descriptor) {
    // ponytail: interactive users share this IPC; use per-session SIDs if concurrent-user isolation is needed.
    constexpr const wchar_t* SDDL = L"D:P(A;;GA;;;SY)(A;;GRGW;;;IU)(A;;0x00100000;;;IU)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            SDDL, SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
    }
    attrs.nLength = sizeof(attrs);
    attrs.lpSecurityDescriptor = descriptor;
    attrs.bInheritHandle = FALSE;
    return true;
}

} // anonymous namespace

Service& Service::get_instance() {
    static Service instance;
    return instance;
}

Service::Service() {
    m_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    m_service_status.dwCurrentState = SERVICE_STOPPED;
    m_service_status.dwControlsAccepted = 0;
    m_service_status.dwWin32ExitCode = NO_ERROR;
    m_service_status.dwServiceSpecificExitCode = 0;
    m_service_status.dwCheckPoint = 0;
    m_service_status.dwWaitHint = 0;
}

Service::~Service() {
    signal_stop();
    if (m_mutex_handle) {
        CloseHandle(m_mutex_handle);
        m_mutex_handle = nullptr;
    }
}

void Service::set_power_change_callback(PowerChangeCallback cb) {
    m_power_callback = std::move(cb);
}

void Service::set_foreground_change_callback(ForegroundChangeCallback cb) {
    m_foreground_callback = std::move(cb);
}

void Service::set_housekeeping_callback(HousekeepingCallback cb) {
    m_housekeeping_callback = std::move(cb);
}

void Service::signal_stop() {
    m_stop_requested = true;
    if (m_stop_event) {
        SetEvent(m_stop_event);
    }
}

bool Service::is_stop_requested() const {
    return m_stop_requested.load();
}

bool Service::is_running() const {
    return m_is_running.load();
}

void Service::update_service_status(DWORD current_state, DWORD win32_exit_code, DWORD wait_hint) {
    m_service_status.dwCurrentState = current_state;
    m_service_status.dwWin32ExitCode = win32_exit_code;
    m_service_status.dwWaitHint = wait_hint;

    if (current_state == SERVICE_START_PENDING) {
        m_service_status.dwControlsAccepted = 0;
    } else if (current_state == SERVICE_RUNNING) {
        m_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT | SERVICE_ACCEPT_SESSIONCHANGE;
    } else if (current_state == SERVICE_STOPPED || current_state == SERVICE_STOP_PENDING) {
        m_service_status.dwControlsAccepted = 0;
    }

    if (m_service_status_handle) {
        SetServiceStatus(m_service_status_handle, &m_service_status);
    }
}

HANDLE Service::acquire_global_mutex() {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!create_ipc_security_attributes(sa, sd)) {
        LOG_ERROR(L"Failed to create global mutex security descriptor: " + Utils::get_last_error_message());
        return nullptr;
    }

    HANDLE hMutex = CreateMutexW(&sa, TRUE, MUTEX_NAME);
    LocalFree(sd);
    if (!hMutex) {
        LOG_ERROR(L"Failed to create global named mutex: " + Utils::get_last_error_message());
        return nullptr;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_WARN(L"Global named mutex already exists. Another instance is running.");
        CloseHandle(hMutex);
        return nullptr;
    }

    m_mutex_handle = hMutex;
    return hMutex;
}

void Service::register_power_notifications(HANDLE recipient, bool is_service_handle) {
    DWORD flags = is_service_handle ? DEVICE_NOTIFY_SERVICE_HANDLE : DEVICE_NOTIFY_WINDOW_HANDLE;

    m_power_notify_acdc = RegisterPowerSettingNotification(recipient, &GUID_ACDC_POWER_SOURCE_VAL, flags);
    if (!m_power_notify_acdc) {
        LOG_WARN(L"RegisterPowerSettingNotification (ACDC) returned: " + Utils::get_last_error_message());
    } else {
        LOG_INFO(L"Registered power setting notification for AC/DC power source transitions.");
    }

    m_power_notify_battery = RegisterPowerSettingNotification(recipient, &GUID_BATTERY_PERCENTAGE_REMAINING_VAL, flags);
    if (!m_power_notify_battery) {
        LOG_WARN(L"RegisterPowerSettingNotification (Battery) returned: " + Utils::get_last_error_message());
    } else {
        LOG_INFO(L"Registered power setting notification for Battery percentage transitions.");
    }
}

void Service::unregister_power_notifications() {
    if (m_power_notify_acdc) {
        UnregisterPowerSettingNotification(m_power_notify_acdc);
        m_power_notify_acdc = nullptr;
    }
    if (m_power_notify_battery) {
        UnregisterPowerSettingNotification(m_power_notify_battery);
        m_power_notify_battery = nullptr;
    }
}

void Service::process_power_broadcast_setting(PPOWERBROADCAST_SETTING setting) {
    if (!setting) return;

    if (IsEqualGUID(setting->PowerSetting, GUID_ACDC_POWER_SOURCE_VAL)) {
        if (setting->DataLength >= sizeof(DWORD)) {
            DWORD val = *reinterpret_cast<const DWORD*>(setting->Data);
            m_current_is_ac = (val == 0); // 0 = AC, 1 = DC, 2 = Hot
            LOG_INFO(L"Power source transition detected: " + std::wstring(m_current_is_ac ? L"AC (Plugged in)" : L"DC (Battery)"));
            if (m_power_callback) {
                m_power_callback(m_current_is_ac, m_current_battery_percent);
            }
        }
    } else if (IsEqualGUID(setting->PowerSetting, GUID_BATTERY_PERCENTAGE_REMAINING_VAL)) {
        if (setting->DataLength >= sizeof(DWORD)) {
            DWORD percent = *reinterpret_cast<const DWORD*>(setting->Data);
            m_current_battery_percent = percent;
            LOG_INFO(L"Battery level update: " + std::to_wstring(percent) + L"%");
            if (m_power_callback) {
                m_power_callback(m_current_is_ac, m_current_battery_percent);
            }
        }
    }
}

void Service::arm_timer(HANDLE hTimer, DWORD period_ms) {
    if (!hTimer || period_ms == 0) return;
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(period_ms) * 10000LL;
    SetWaitableTimer(hTimer, &due, static_cast<LONG>(period_ms), nullptr, nullptr, FALSE);
}

bool Service::create_fg_ipc() {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!create_ipc_security_attributes(sa, sd)) {
        LOG_WARN(L"Failed to create foreground IPC security descriptor: " + Utils::get_last_error_message());
        return false;
    }

    m_fg_map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SharedFgState), FG_MAP_NAME);
    if (!m_fg_map) {
        LocalFree(sd);
        LOG_WARN(L"Failed to create foreground IPC mapping: " + Utils::get_last_error_message());
        return false;
    }
    m_fg_event = CreateEventW(&sa, FALSE, FALSE, FG_EVENT_NAME);
    LocalFree(sd);
    m_fg_view = static_cast<SharedFgState*>(MapViewOfFile(m_fg_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedFgState)));
    if (m_fg_view) {
        *m_fg_view = SharedFgState{};
    }
    if (!m_fg_event) {
        LOG_WARN(L"Failed to create foreground IPC event: " + Utils::get_last_error_message());
        return false;
    }
    return m_fg_view != nullptr;
}

void Service::close_fg_ipc() {
    stop_session_watch();
    if (m_fg_view) {
        UnmapViewOfFile(m_fg_view);
        m_fg_view = nullptr;
    }
    if (m_fg_map) {
        CloseHandle(m_fg_map);
        m_fg_map = nullptr;
    }
    if (m_fg_event) {
        CloseHandle(m_fg_event);
        m_fg_event = nullptr;
    }
}

bool Service::create_ui_ipc() {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!create_ipc_security_attributes(sa, sd)) {
        LOG_WARN(L"Failed to create UI IPC security descriptor: " + Utils::get_last_error_message());
        return false;
    }

    m_ui_map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SharedUiState), surface_optimizer::UI_MAP_NAME);
    LocalFree(sd);
    if (!m_ui_map) {
        LOG_WARN(L"Failed to create UI IPC mapping: " + Utils::get_last_error_message());
        return false;
    }
    m_ui_view = static_cast<SharedUiState*>(MapViewOfFile(m_ui_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedUiState)));
    if (!m_ui_view) {
        return false;
    }
    *m_ui_view = SharedUiState{};
    m_ui_view->optimizer_on = 1;
    m_ui_view->autostart = query_service_autostart() ? 1u : 0u;
    return true;
}

void Service::close_ui_ipc() {
    if (m_ui_view) {
        m_ui_view->stop_watch = 1;
        UnmapViewOfFile(m_ui_view);
        m_ui_view = nullptr;
    }
    if (m_ui_map) {
        CloseHandle(m_ui_map);
        m_ui_map = nullptr;
    }
}

bool Service::query_service_autostart() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        return false;
    }
    SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_QUERY_CONFIG);
    if (!hService) {
        CloseServiceHandle(hSCM);
        return false;
    }
    DWORD needed = 0;
    QueryServiceConfigW(hService, nullptr, 0, &needed);
    std::vector<uint8_t> buf(needed ? needed : 1);
    bool auto_start = true;
    if (QueryServiceConfigW(hService, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data()), needed, &needed)) {
        auto* cfg = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buf.data());
        auto_start = (cfg->dwStartType == SERVICE_AUTO_START);
    }
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return auto_start;
}

bool Service::set_service_autostart(bool enabled) {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        LOG_WARN(L"set_service_autostart: OpenSCManager failed: " + Utils::get_last_error_message());
        return false;
    }
    SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_CHANGE_CONFIG);
    if (!hService) {
        CloseServiceHandle(hSCM);
        LOG_WARN(L"set_service_autostart: OpenService failed: " + Utils::get_last_error_message());
        return false;
    }
    const DWORD type = enabled ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
    const BOOL ok = ChangeServiceConfigW(
        hService,
        SERVICE_NO_CHANGE,
        type,
        SERVICE_NO_CHANGE,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    if (!ok) {
        LOG_WARN(L"ChangeServiceConfig autostart failed: " + Utils::get_last_error_message());
        return false;
    }
    LOG_INFO(std::wstring(L"Service autostart ") + (enabled ? L"enabled" : L"disabled") + L" from tray.");
    return true;
}

void Service::consume_ui_commands() {
    if (!m_ui_view) {
        return;
    }
    if (m_ui_view->cmd_seq != m_ui_view->ack_seq) {
        if (m_ui_view->cmd_value > 1) {
            LOG_WARN(L"Ignored invalid UI command value.");
        } else if (m_ui_view->cmd == UI_CMD_SET_OPTIMIZER) {
            const bool on = m_ui_view->cmd_value != 0;
            PowerManager::get_instance().set_paused(!on);
            if (on) {
                ProcessGuardian::get_instance().enable_enforcement();
            } else {
                ProcessGuardian::get_instance().disable_enforcement();
            }
            LOG_INFO(std::wstring(L"Tray optimizer ") + (on ? L"ON" : L"OFF"));
        } else if (m_ui_view->cmd == UI_CMD_SET_AUTOSTART) {
            set_service_autostart(m_ui_view->cmd_value != 0);
        }
        m_ui_view->ack_seq = m_ui_view->cmd_seq;
    }
    m_ui_view->optimizer_on = PowerManager::get_instance().is_paused() ? 0u : 1u;
    m_ui_view->autostart = query_service_autostart() ? 1u : 0u;
}

void Service::start_session_watch() {
    if (m_fg_watch_process) {
        DWORD code = 0;
        if (GetExitCodeProcess(m_fg_watch_process, &code) && code == STILL_ACTIVE) {
            return;
        }
        CloseHandle(m_fg_watch_process);
        m_fg_watch_process = nullptr;
    }

    DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF) {
        return;
    }
    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(session, &hToken)) {
        LOG_DEBUG(L"WTSQueryUserToken failed (no interactive session yet)");
        return;
    }

    std::wstring exe = Utils::get_executable_path();
    std::wstring cmd = L"\"" + exe + L"\" --foreground-watch " + std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessAsUserW(
        hToken,
        exe.c_str(),
        cmd_buf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    CloseHandle(hToken);
    if (!ok) {
        LOG_WARN(L"Failed to start session foreground watch: " + Utils::get_last_error_message());
        return;
    }
    CloseHandle(pi.hThread);
    m_fg_watch_process = pi.hProcess;
    LOG_INFO(L"Started user-session foreground watch PID=" + std::to_wstring(pi.dwProcessId));
}

void Service::stop_session_watch() {
    if (!m_fg_watch_process) return;
    if (m_ui_view) {
        m_ui_view->stop_watch = 1;
    }
    if (WaitForSingleObject(m_fg_watch_process, 2000) != WAIT_OBJECT_0) {
        TerminateProcess(m_fg_watch_process, 0);
        WaitForSingleObject(m_fg_watch_process, 1000);
    }
    CloseHandle(m_fg_watch_process);
    m_fg_watch_process = nullptr;
}

bool Service::consume_fg_state() {
    if (!m_fg_view) return false;
    uint32_t pid = m_fg_view->pid;
    uint32_t idle_ms = m_fg_view->last_input_idle_ms;
    PowerManager::get_instance().set_last_input_idle_ms(idle_ms);
    if (pid != 0 && pid != m_last_seen_fg_pid) {
        m_last_seen_fg_pid = pid;
        std::wstring name = m_fg_view->image_name;
        if (name.empty()) {
            name = Utils::get_process_name_by_pid(pid);
        }
        if (m_foreground_callback) {
            m_foreground_callback(pid, name);
        }
        return true;
    }
    return false;
}

HWND Service::create_message_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Service::power_sink_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SurfaceOptimizerPowerSink";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

LRESULT CALLBACK Service::power_sink_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE && lParam) {
        Service::get_instance().process_power_broadcast_setting(reinterpret_cast<PPOWERBROADCAST_SETTING>(lParam));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

namespace {
Service::SharedFgState* g_watch_view = nullptr;
HANDLE g_watch_event = nullptr;

void publish_watch_state(DWORD pid) {
    if (!g_watch_view) return;
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    uint32_t idle_ms = 0xFFFFFFFFu;
    if (GetLastInputInfo(&info)) {
        idle_ms = GetTickCount() - info.dwTime;
    }
    g_watch_view->pid = pid;
    g_watch_view->last_input_idle_ms = idle_ms;
    std::wstring name = Utils::get_process_name_by_pid(pid);
    size_t ncopy = name.size();
    if (ncopy > 63) ncopy = 63;
    memcpy(g_watch_view->image_name, name.c_str(), ncopy * sizeof(wchar_t));
    g_watch_view->image_name[ncopy] = L'\0';
    g_watch_view->seq += 1;
    if (g_watch_event) {
        SetEvent(g_watch_event);
    }
}
} // namespace

void CALLBACK Service::watch_win_event_proc(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD /*event_thread*/, DWORD /*event_time*/) {
    if (event != EVENT_SYSTEM_FOREGROUND || id_object != OBJID_WINDOW || id_child != INDEXID_CONTAINER) return;
    if (!hwnd || !IsWindow(hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        publish_watch_state(pid);
    }
}

int Service::run_foreground_watch(DWORD parent_pid) {
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, FG_MAP_NAME);
    HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, FG_EVENT_NAME);
    if (!hMap || !hEvt) {
        return 1;
    }
    g_watch_view = static_cast<SharedFgState*>(MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedFgState)));
    g_watch_event = hEvt;
    if (!g_watch_view) {
        return 2;
    }

    SharedUiState* ui = nullptr;
    HANDLE hUiMap = nullptr;
    HWND tray_hwnd = nullptr;
    for (int i = 0; i < 50 && !ui; ++i) {
        hUiMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, UI_MAP_NAME);
        if (hUiMap) {
            ui = static_cast<SharedUiState*>(MapViewOfFile(hUiMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedUiState)));
        }
        if (!ui) {
            Sleep(100);
        }
    }
    tray_hwnd = tray_create_window();
    if (tray_hwnd) {
        tray_install(tray_hwnd, ui);
    }

    HWINEVENTHOOK hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, watch_win_event_proc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    HANDLE hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (hTimer) {
        LARGE_INTEGER due{};
        due.QuadPart = -1000000LL;
        SetWaitableTimer(hTimer, &due, 100, nullptr, nullptr, FALSE);
    }

    HANDLE waits[2];
    DWORD n = 0;
    DWORD parent_index = 0xFFFFFFFF;
    if (hParent) {
        parent_index = n;
        waits[n++] = hParent;
    }
    if (hTimer) {
        waits[n++] = hTimer;
    }

    bool running = true;
    while (running) {
        DWORD wr = MsgWaitForMultipleObjectsEx(n, waits, INFINITE, QS_ALLINPUT, MWMO_ALERTABLE);
        if (parent_index != 0xFFFFFFFF && wr == WAIT_OBJECT_0 + parent_index) {
            break;
        }
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid) publish_watch_state(pid);
        if (ui) {
            if (ui->stop_watch) {
                running = false;
            }
            tray_update(ui->optimizer_on != 0, ui->autostart != 0);
        }

        if (wr == WAIT_OBJECT_0 + n) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    running = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    tray_remove();
    if (tray_hwnd) {
        DestroyWindow(tray_hwnd);
    }
    if (ui) {
        UnmapViewOfFile(ui);
    }
    if (hUiMap) {
        CloseHandle(hUiMap);
    }
    if (hook) UnhookWinEvent(hook);
    if (hTimer) CloseHandle(hTimer);
    if (hParent) CloseHandle(hParent);
    UnmapViewOfFile(g_watch_view);
    CloseHandle(hMap);
    CloseHandle(hEvt);
    g_watch_view = nullptr;
    g_watch_event = nullptr;
    return 0;
}

void CALLBACK Service::win_event_proc(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD /*event_thread*/, DWORD /*event_time*/) {
    if (event == EVENT_SYSTEM_FOREGROUND && id_object == OBJID_WINDOW && id_child == INDEXID_CONTAINER) {
        if (hwnd && IsWindow(hwnd)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != 0) {
                std::wstring proc_name = Utils::get_process_name_by_pid(pid);
                LOG_DEBUG(L"Foreground window changed: PID=" + std::to_wstring(pid) + L" (" + proc_name + L")");
                auto& svc = Service::get_instance();
                if (svc.m_foreground_callback) {
                    svc.m_foreground_callback(pid, proc_name);
                }
            }
        }
    }
}

BOOL WINAPI Service::console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            LOG_INFO(L"Console shutdown/break signal received. Requesting clean termination...");
            Service::get_instance().signal_stop();
            return TRUE;
        default:
            return FALSE;
    }
}

void WINAPI Service::service_main(DWORD /*argc*/, LPWSTR* /*argv*/) {
    auto& svc = Service::get_instance();
    svc.m_service_status_handle = RegisterServiceCtrlHandlerExW(
        SERVICE_NAME,
        Service::service_handler_ex,
        nullptr
    );

    if (!svc.m_service_status_handle) {
        LOG_ERROR(L"RegisterServiceCtrlHandlerExW failed: " + Utils::get_last_error_message());
        return;
    }

    svc.update_service_status(SERVICE_START_PENDING, NO_ERROR, 3000);
    LOG_INFO(L"SurfaceOptimizer ServiceMain started.");

    int res = svc.run_daemon_loop(false);
    svc.update_service_status(SERVICE_STOPPED, res == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
    LOG_INFO(L"SurfaceOptimizer ServiceMain finished with code: " + std::to_wstring(res));
}

DWORD WINAPI Service::service_handler_ex(DWORD control, DWORD event_type, LPVOID event_data, LPVOID /*context*/) {
    auto& svc = Service::get_instance();

    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            LOG_INFO(L"SCM STOP/SHUTDOWN control event received.");
            svc.update_service_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            svc.signal_stop();
            return NO_ERROR;

        case SERVICE_CONTROL_POWEREVENT:
            if (event_type == PBT_POWERSETTINGCHANGE && event_data) {
                svc.process_power_broadcast_setting(reinterpret_cast<PPOWERBROADCAST_SETTING>(event_data));
            }
            return NO_ERROR;

        case SERVICE_CONTROL_SESSIONCHANGE:
            if (event_type == WTS_SESSION_LOGON || event_type == WTS_CONSOLE_CONNECT || event_type == WTS_SESSION_UNLOCK) {
                svc.start_session_watch();
            } else if (event_type == WTS_SESSION_LOGOFF || event_type == WTS_CONSOLE_DISCONNECT) {
                svc.stop_session_watch();
            }
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

int Service::run_as_service() {
    SERVICE_TABLE_ENTRYW dispatch_table[] = {
        { const_cast<LPWSTR>(SERVICE_NAME), Service::service_main },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(dispatch_table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            LOG_ERROR(L"StartServiceCtrlDispatcher failed (1063): Program was not started by Service Control Manager. Use --interactive to run in console.");
        } else {
            LOG_ERROR(L"StartServiceCtrlDispatcher failed: " + Utils::get_win32_error_message(err));
        }
        return static_cast<int>(err);
    }
    return 0;
}

int Service::run_interactive() {
    SetConsoleCtrlHandler(Service::console_ctrl_handler, TRUE);
    LOG_INFO(L"Starting SurfaceOptimizer daemon in interactive console mode...");
    return run_daemon_loop(true);
}

int Service::run_daemon_loop(bool is_interactive) {
    // 1. Enable OS privileges
    Utils::enable_required_privileges();

    // 2. Acquire single instance mutex
    HANDLE hMutex = acquire_global_mutex();
    if (!hMutex) {
        std::wcerr << L"Error: Another instance of SurfaceOptimizer is already running.\n";
        return 1;
    }

    // 3. Create stop event
    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop_event) {
        LOG_ERROR(L"Failed to create stop event: " + Utils::get_last_error_message());
        return 2;
    }

    // 4. Create waitable timer for periodic housekeeping
    auto& config = Config::get_instance();
    HANDLE hTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    DWORD current_period_ms = config.power.busy_poll_interval_ms;
    if (hTimer) {
        arm_timer(hTimer, current_period_ms);
    }

    // 5. Initialize Subsystems (M2: PowerManager, M3: MemoryManager)
    PowerManager::get_instance().initialize();
    MemoryManager::get_instance().initialize();
    ProcessGuardian::get_instance().initialize();
    ProcessGuardian::get_instance().enable_enforcement();

    set_power_change_callback([](bool is_ac, uint32_t battery_percent) {
        PowerManager::get_instance().on_power_source_changed(is_ac ? PowerSource::AC : PowerSource::Battery);
        PowerManager::get_instance().on_battery_percent_changed(battery_percent);
    });

    set_foreground_change_callback([](DWORD pid, const std::wstring& image_name) {
        PowerManager::get_instance().on_foreground_process_changed(pid, image_name);
        MemoryManager::get_instance().on_foreground_process_changed(pid, image_name);
        ProcessGuardian::get_instance().on_foreground_process_changed(pid, image_name);
    });

    create_fg_ipc();
    create_ui_ipc();

    // 6. Register notifications
    if (is_interactive) {
        m_foreground_hook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, win_event_proc,
            0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
        );
        if (!m_foreground_hook) {
            LOG_WARN(L"Failed to install SetWinEventHook for foreground window focus: " + Utils::get_last_error_message());
        } else {
            LOG_INFO(L"Zero-polling WinEvent hook installed for instant foreground focus tracking.");
        }
        m_msg_window = create_message_window();
        if (m_msg_window) {
            register_power_notifications(m_msg_window, false);
        }
        m_tray_window = tray_create_window();
        if (m_tray_window) {
            tray_install(m_tray_window, m_ui_view);
        }
    } else {
        register_power_notifications(m_service_status_handle, true);
        start_session_watch();
    }

    m_is_running = true;
    if (!is_interactive) {
        update_service_status(SERVICE_RUNNING);
    }

    LOG_INFO(L"SurfaceOptimizer daemon core loop active. Running state: ACTIVE.");

    HANDLE wait_handles[3] = { m_stop_event, hTimer, m_fg_event };
    DWORD num_handles = 1;
    if (hTimer) num_handles = 2;
    if (m_fg_event) num_handles = (hTimer ? 3 : 2);

    auto last_slow = std::chrono::steady_clock::now();

    while (!m_stop_requested.load()) {
        DWORD wait_result = MsgWaitForMultipleObjectsEx(
            num_handles,
            wait_handles,
            INFINITE,
            QS_ALLINPUT,
            MWMO_ALERTABLE
        );

        if (wait_result == WAIT_OBJECT_0) {
            LOG_INFO(L"Stop event signaled. Initiating clean termination...");
            break;
        } else if (hTimer && wait_result == WAIT_OBJECT_0 + 1) {
            consume_fg_state();
            consume_ui_commands();
            if (is_interactive) {
                tray_update(!PowerManager::get_instance().is_paused(),
                            m_ui_view ? m_ui_view->autostart != 0 : true);
            }

            const bool paused = PowerManager::get_instance().is_paused();
            if (!paused) {
                PowerManager::get_instance().on_housekeeping();
            }

            DWORD want = paused ? config.power.idle_poll_interval_ms
                                : PowerManager::get_instance().desired_poll_interval_ms();
            if (want != current_period_ms && want != 0) {
                current_period_ms = want;
                arm_timer(hTimer, current_period_ms);
            }

            auto now = std::chrono::steady_clock::now();
            DWORD slow_ms = m_current_is_ac
                ? config.daemon.housekeeping_interval_ac_ms
                : config.daemon.housekeeping_interval_dc_ms;
            if (!paused &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_slow).count() >= static_cast<int64_t>(slow_ms)) {
                last_slow = now;
                MemoryManager::get_instance().on_housekeeping(
                    MemoryManager::get_instance().get_current_foreground_pid(),
                    m_current_is_ac
                );
                ProcessGuardian::get_instance().on_housekeeping(
                    MemoryManager::get_instance().get_current_foreground_pid()
                );
            }
        } else if (m_fg_event && wait_result == WAIT_OBJECT_0 + (hTimer ? 2 : 1)) {
            consume_fg_state();
        } else if (wait_result == WAIT_OBJECT_0 + num_handles) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    m_stop_requested = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        } else if (wait_result == WAIT_FAILED) {
            LOG_ERROR(L"MsgWaitForMultipleObjectsEx failed: " + Utils::get_last_error_message());
            break;
        }
    }

    // Clean up resources
    LOG_INFO(L"Cleaning up daemon resources and unregistering hooks...");

    if (m_foreground_hook) {
        UnhookWinEvent(m_foreground_hook);
        m_foreground_hook = nullptr;
    }

    unregister_power_notifications();
    tray_remove();
    if (m_tray_window) {
        DestroyWindow(m_tray_window);
        m_tray_window = nullptr;
    }
    if (m_msg_window) {
        DestroyWindow(m_msg_window);
        m_msg_window = nullptr;
    }
    close_fg_ipc();
    close_ui_ipc();

    if (hTimer) {
        CancelWaitableTimer(hTimer);
        CloseHandle(hTimer);
    }

    if (m_stop_event) {
        CloseHandle(m_stop_event);
        m_stop_event = nullptr;
    }

    ProcessGuardian::get_instance().shutdown();
    MemoryManager::get_instance().shutdown();
    PowerManager::get_instance().shutdown();

    m_is_running = false;
    LOG_INFO(L"SurfaceOptimizer daemon shut down cleanly.");
    return 0;
}

bool Service::install_service() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        std::wcerr << L"Failed to connect to Service Control Manager: " << Utils::get_last_error_message() << std::endl;
        return false;
    }

    std::wstring binary_path = Utils::get_executable_path();
    std::wstring service_command = L"\"" + binary_path + L"\" --daemon";

    SC_HANDLE hService = CreateServiceW(
        hSCM,
        SERVICE_NAME,
        SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        service_command.c_str(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    );

    if (!hService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            std::wcout << L"Service '" << SERVICE_NAME << L"' is already registered. Updating configuration...\n";
            hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_ALL_ACCESS);
        }
        if (!hService) {
            std::wcerr << L"Failed to create service: " << Utils::get_win32_error_message(err) << std::endl;
            CloseServiceHandle(hSCM);
            return false;
        }
    }

    // Set description
    SERVICE_DESCRIPTIONW desc{};
    desc.lpDescription = const_cast<LPWSTR>(SERVICE_DESCRIPTION);
    ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &desc);

    // Set failure actions (auto restart on failure)
    SC_ACTION actions[3];
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 5000;  // 5 seconds
    actions[1].Type = SC_ACTION_RESTART;
    actions[1].Delay = 10000; // 10 seconds
    actions[2].Type = SC_ACTION_RESTART;
    actions[2].Delay = 30000; // 30 seconds

    SERVICE_FAILURE_ACTIONSW sfa{};
    sfa.dwResetPeriod = 86400; // Reset failure counter after 24 hours
    sfa.lpRebootMsg = nullptr;
    sfa.lpCommand = nullptr;
    sfa.cActions = 3;
    sfa.lpsaActions = actions;

    if (!ChangeServiceConfig2W(hService, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa)) {
        LOG_WARN(L"Failed to configure service restart failure actions: " + Utils::get_last_error_message());
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);

    std::wcout << L"Service '" << SERVICE_NAME << L"' installed successfully.\n";
    std::wcout << L"Display Name: " << SERVICE_DISPLAY_NAME << L"\n";
    std::wcout << L"Binary Path:  " << service_command << L"\n";
    std::wcout << L"Start Type:   Automatic (Auto-Restart enabled)\n";
    return true;
}

bool Service::uninstall_service() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        std::wcerr << L"Failed to connect to Service Control Manager: " << Utils::get_last_error_message() << std::endl;
        return false;
    }

    SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!hService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::wcout << L"Service '" << SERVICE_NAME << L"' is not installed.\n";
            CloseServiceHandle(hSCM);
            return true;
        }
        std::wcerr << L"Failed to open service: " << Utils::get_win32_error_message(err) << std::endl;
        CloseServiceHandle(hSCM);
        return false;
    }

    // Stop service if running
    SERVICE_STATUS status{};
    if (ControlService(hService, SERVICE_CONTROL_STOP, &status)) {
        std::wcout << L"Stopping service '" << SERVICE_NAME << L"'...\n";
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (QueryServiceStatus(hService, &status) && status.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
        }
    }

    if (!DeleteService(hService)) {
        std::wcerr << L"Failed to delete service: " << Utils::get_last_error_message() << std::endl;
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return false;
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);

    std::wcout << L"Service '" << SERVICE_NAME << L"' uninstalled successfully.\n";
    return true;
}

bool Service::start_service() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        std::wcerr << L"Failed to connect to Service Control Manager: " << Utils::get_last_error_message() << std::endl;
        return false;
    }

    SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hService) {
        std::wcerr << L"Failed to open service: " << Utils::get_last_error_message() << std::endl;
        CloseServiceHandle(hSCM);
        return false;
    }

    SERVICE_STATUS status{};
    if (QueryServiceStatus(hService, &status) && status.dwCurrentState == SERVICE_RUNNING) {
        std::wcout << L"Service '" << SERVICE_NAME << L"' is already running.\n";
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return true;
    }

    if (!StartServiceW(hService, 0, nullptr)) {
        std::wcerr << L"Failed to start service: " << Utils::get_last_error_message() << std::endl;
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return false;
    }

    std::wcout << L"Starting service '" << SERVICE_NAME << L"'...\n";
    bool started = false;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (QueryServiceStatus(hService, &status) && status.dwCurrentState == SERVICE_RUNNING) {
            started = true;
            break;
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);

    if (started) {
        std::wcout << L"Service '" << SERVICE_NAME << L"' is now RUNNING.\n";
        return true;
    } else {
        std::wcerr << L"Timeout waiting for service to reach RUNNING state.\n";
        return false;
    }
}

bool Service::stop_service() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        std::wcerr << L"Failed to connect to Service Control Manager: " << Utils::get_last_error_message() << std::endl;
        return false;
    }

    SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hService) {
        std::wcerr << L"Failed to open service: " << Utils::get_last_error_message() << std::endl;
        CloseServiceHandle(hSCM);
        return false;
    }

    SERVICE_STATUS status{};
    if (QueryServiceStatus(hService, &status) && status.dwCurrentState == SERVICE_STOPPED) {
        std::wcout << L"Service '" << SERVICE_NAME << L"' is already stopped.\n";
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return true;
    }

    if (!ControlService(hService, SERVICE_CONTROL_STOP, &status)) {
        std::wcerr << L"Failed to send STOP command to service: " << Utils::get_last_error_message() << std::endl;
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return false;
    }

    std::wcout << L"Stopping service '" << SERVICE_NAME << L"'...\n";
    bool stopped = false;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (QueryServiceStatus(hService, &status) && status.dwCurrentState == SERVICE_STOPPED) {
            stopped = true;
            break;
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);

    if (stopped) {
        std::wcout << L"Service '" << SERVICE_NAME << L"' is now STOPPED.\n";
        return true;
    } else {
        std::wcerr << L"Timeout waiting for service to reach STOPPED state.\n";
        return false;
    }
}

ServiceInfo Service::query_service_info() {
    ServiceInfo info{};

    // Check Mutex state first (works for both service & interactive daemon)
    HANDLE hMutexCheck = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME);
    if (hMutexCheck) {
        info.is_daemon_mutex_held = true;
        CloseHandle(hMutexCheck);
    }

    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        info.status_text = L"Unable to connect to Service Control Manager (" + Utils::get_last_error_message() + L")";
        return info;
    }

    SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!hService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            info.state = ServiceState::NotInstalled;
            info.status_text = L"Service is NOT installed.";
        } else {
            info.status_text = L"Failed to query service: " + Utils::get_win32_error_message(err);
        }
        CloseServiceHandle(hSCM);
        return info;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes_needed = 0;
    if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes_needed)) {
        info.pid = ssp.dwProcessId;
        info.win32_exit_code = ssp.dwWin32ExitCode;

        switch (ssp.dwCurrentState) {
            case SERVICE_STOPPED:
                info.state = ServiceState::Stopped;
                info.status_text = L"STOPPED";
                break;
            case SERVICE_START_PENDING:
                info.state = ServiceState::StartPending;
                info.status_text = L"START PENDING";
                break;
            case SERVICE_STOP_PENDING:
                info.state = ServiceState::StopPending;
                info.status_text = L"STOP PENDING";
                break;
            case SERVICE_RUNNING:
                info.state = ServiceState::Running;
                info.status_text = L"RUNNING";
                break;
            case SERVICE_CONTINUE_PENDING:
                info.state = ServiceState::ContinuePending;
                info.status_text = L"CONTINUE PENDING";
                break;
            case SERVICE_PAUSE_PENDING:
                info.state = ServiceState::PausePending;
                info.status_text = L"PAUSE PENDING";
                break;
            case SERVICE_PAUSED:
                info.state = ServiceState::Paused;
                info.status_text = L"PAUSED";
                break;
            default:
                info.state = ServiceState::Unknown;
                info.status_text = L"UNKNOWN";
                break;
        }
    }

    // Query config for binary path
    DWORD config_size = 0;
    QueryServiceConfigW(hService, nullptr, 0, &config_size);
    if (config_size > 0) {
        std::vector<BYTE> buffer(config_size);
        auto* pConfig = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(buffer.data());
        if (QueryServiceConfigW(hService, pConfig, config_size, &config_size)) {
            if (pConfig->lpBinaryPathName) {
                info.binary_path = pConfig->lpBinaryPathName;
            }
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return info;
}

} // namespace surface_optimizer
