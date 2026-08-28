#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace surface_optimizer {

class Utils {
public:
    // Privilege management
    static bool enable_privilege(const wchar_t* privilege_name);
    static bool enable_required_privileges();
    static bool is_running_as_admin();
    static bool is_running_as_system();

    // String conversions
    static std::wstring utf8_to_wide(const std::string& utf8_str);
    static std::string wide_to_utf8(const std::wstring& wide_str);
    static std::wstring guid_to_wstring(const GUID& guid);

    // Path helpers
    static std::wstring get_executable_path();
    static std::wstring get_executable_dir();

    // Error formatting
    static std::wstring get_win32_error_message(DWORD error_code);
    static std::wstring get_last_error_message();

    // Timing
    static uint64_t get_current_time_millis();
    static double get_high_resolution_timestamp_sec();

    // Process helpers
    static std::wstring get_process_name_by_pid(DWORD pid);
};

} // namespace surface_optimizer
