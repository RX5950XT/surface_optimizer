#pragma once

#include <windows.h>
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <vector>

namespace surface_optimizer {

enum class ServiceState {
    NotInstalled,
    Stopped,
    StartPending,
    StopPending,
    Running,
    ContinuePending,
    PausePending,
    Paused,
    Unknown
};

struct ServiceInfo {
    ServiceState state = ServiceState::NotInstalled;
    DWORD pid = 0;
    DWORD win32_exit_code = 0;
    std::wstring binary_path;
    std::wstring status_text;
    bool is_daemon_mutex_held = false;
};

// Callback signatures for extensible subsystem integration (M2..M5)
using PowerChangeCallback = std::function<void(bool is_ac, uint32_t battery_percent)>;
using ForegroundChangeCallback = std::function<void(DWORD pid, const std::wstring& image_name)>;
using HousekeepingCallback = std::function<void()>;

class Service {
public:
    static constexpr const wchar_t* SERVICE_NAME = L"SurfaceOptimizer";
    static constexpr const wchar_t* SERVICE_DISPLAY_NAME = L"Surface Pro 7 Performance & Energy Optimizer";
    static constexpr const wchar_t* SERVICE_DESCRIPTION = L"Native ultra-lightweight power, memory, and process optimization daemon for Surface Pro 7.";
    static constexpr const wchar_t* MUTEX_NAME = L"Global\\SurfaceOptimizerDaemonMutex";
    static constexpr const wchar_t* FG_MAP_NAME = L"Global\\SurfaceOptimizerFgState";
    static constexpr const wchar_t* FG_EVENT_NAME = L"Global\\SurfaceOptimizerFgEvent";

    struct SharedFgState {
        uint32_t pid = 0;
        uint32_t last_input_idle_ms = 0xFFFFFFFFu;
        uint32_t seq = 0;
        wchar_t image_name[64] = {};
    };

    static Service& get_instance();

    // SCM Service Management (CLI commands)
    bool install_service();
    bool uninstall_service();
    bool start_service();
    bool stop_service();
    ServiceInfo query_service_info();

    // Execution Modes
    int run_as_service();
    int run_interactive();
    static int run_foreground_watch(DWORD parent_pid);

    // Extensible Component Registration (for M2..M5)
    void set_power_change_callback(PowerChangeCallback cb);
    void set_foreground_change_callback(ForegroundChangeCallback cb);
    void set_housekeeping_callback(HousekeepingCallback cb);

    // Lifecycle
    void signal_stop();
    bool is_stop_requested() const;
    bool is_running() const;

private:
    Service();
    ~Service();

    // SCM callbacks
    static void WINAPI service_main(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI service_handler_ex(DWORD control, DWORD event_type, LPVOID event_data, LPVOID context);
    static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type);

    // Internal loop implementation
    int run_daemon_loop(bool is_interactive);
    void update_service_status(DWORD current_state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint = 0);
    HANDLE acquire_global_mutex();

    // Event hooks & power notification handles
    void register_power_notifications(HANDLE recipient, bool is_service_handle);
    void unregister_power_notifications();
    void process_power_broadcast_setting(PPOWERBROADCAST_SETTING setting);
    bool create_fg_ipc();
    void close_fg_ipc();
    void start_session_watch();
    void stop_session_watch();
    bool consume_fg_state();
    bool create_ui_ipc();
    void close_ui_ipc();
    void consume_ui_commands();
    bool query_service_autostart();
    bool set_service_autostart(bool enabled);
    void arm_timer(HANDLE hTimer, DWORD period_ms);
    HWND create_message_window();

    static LRESULT CALLBACK power_sink_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK watch_win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD event_thread, DWORD event_time);

    SERVICE_STATUS_HANDLE m_service_status_handle = nullptr;
    SERVICE_STATUS m_service_status{};
    HANDLE m_stop_event = nullptr;
    HANDLE m_mutex_handle = nullptr;
    HPOWERNOTIFY m_power_notify_acdc = nullptr;
    HPOWERNOTIFY m_power_notify_battery = nullptr;
    HWINEVENTHOOK m_foreground_hook = nullptr;
    HWND m_msg_window = nullptr;
    HANDLE m_fg_map = nullptr;
    HANDLE m_fg_event = nullptr;
    SharedFgState* m_fg_view = nullptr;
    HANDLE m_ui_map = nullptr;
    struct SharedUiState* m_ui_view = nullptr;
    HWND m_tray_window = nullptr;
    HANDLE m_fg_watch_process = nullptr;
    uint32_t m_last_seen_fg_pid = 0;
    std::atomic<bool> m_stop_requested{false};
    std::atomic<bool> m_is_running{false};

    bool m_current_is_ac = true;
    uint32_t m_current_battery_percent = 100;

    PowerChangeCallback m_power_callback;
    ForegroundChangeCallback m_foreground_callback;
    HousekeepingCallback m_housekeeping_callback;

    static void CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD event_thread, DWORD event_time);
};

} // namespace surface_optimizer
