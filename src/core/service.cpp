#include "core/service.hpp"
#include "core/logger.hpp"
#include "core/config.hpp"
#include "core/utils.hpp"
#include "optimizer/power_manager.hpp"
#include "optimizer/memory_manager.hpp"
#include "optimizer/process_guardian.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <sddl.h>

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
        m_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT;
    } else if (current_state == SERVICE_STOPPED || current_state == SERVICE_STOP_PENDING) {
        m_service_status.dwControlsAccepted = 0;
    }

    if (m_service_status_handle) {
        SetServiceStatus(m_service_status_handle, &m_service_status);
    }
}

HANDLE Service::acquire_global_mutex() {
    SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    HANDLE hMutex = CreateMutexW(&sa, TRUE, MUTEX_NAME);
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
    if (hTimer) {
        LARGE_INTEGER due_time;
        due_time.QuadPart = -10000000LL; // 1 second initial delay
        DWORD period_ms = config.daemon.housekeeping_interval_ac_ms;
        SetWaitableTimer(hTimer, &due_time, period_ms, nullptr, nullptr, FALSE);
    }

    // 5. Initialize Subsystems (M2: PowerManager, M3: MemoryManager)
    PowerManager::get_instance().initialize();
    MemoryManager::get_instance().initialize();
    ProcessGuardian::get_instance().initialize();

    set_power_change_callback([](bool is_ac, uint32_t battery_percent) {
        PowerManager::get_instance().on_power_source_changed(is_ac ? PowerSource::AC : PowerSource::Battery);
        PowerManager::get_instance().on_battery_percent_changed(battery_percent);
    });

    set_foreground_change_callback([](DWORD pid, const std::wstring& image_name) {
        PowerManager::get_instance().on_foreground_process_changed(pid, image_name);
        MemoryManager::get_instance().on_foreground_process_changed(pid, image_name);
        ProcessGuardian::get_instance().on_foreground_process_changed(pid, image_name);
    });

    set_housekeeping_callback([this]() {
        PowerManager::get_instance().on_housekeeping();
        MemoryManager::get_instance().on_housekeeping(
            MemoryManager::get_instance().get_current_foreground_pid(),
            m_current_is_ac
        );
        ProcessGuardian::get_instance().on_housekeeping(
            MemoryManager::get_instance().get_current_foreground_pid()
        );
    });

    // 6. Register notifications
    if (is_interactive) {
        // In interactive mode, install WinEvent hook for foreground focus
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
    } else {
        // In SCM mode, register power setting notification on service status handle
        register_power_notifications(m_service_status_handle, true);
    }

    m_is_running = true;
    if (!is_interactive) {
        update_service_status(SERVICE_RUNNING);
    }

    LOG_INFO(L"SurfaceOptimizer daemon core loop active. Running state: ACTIVE.");

    // Event-driven message/wait loop
    HANDLE wait_handles[2] = { m_stop_event, hTimer };
    DWORD num_handles = (hTimer != nullptr) ? 2 : 1;

    while (!m_stop_requested.load()) {
        DWORD wait_result = MsgWaitForMultipleObjectsEx(
            num_handles,
            wait_handles,
            INFINITE,
            QS_ALLINPUT,
            MWMO_ALERTABLE
        );

        if (wait_result == WAIT_OBJECT_0) {
            // Stop event signaled
            LOG_INFO(L"Stop event signaled. Initiating clean termination...");
            break;
        } else if (wait_result == WAIT_OBJECT_0 + 1) {
            // Waitable timer fired - run periodic housekeeper
            if (m_housekeeping_callback) {
                m_housekeeping_callback();
            }
        } else if (wait_result == WAIT_OBJECT_0 + num_handles) {
            // Windows messages pending (for WinEvent hooks & power messages)
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
