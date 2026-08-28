#include "core/utils.hpp"
#include "core/logger.hpp"
#include <chrono>
#include <vector>

namespace surface_optimizer {

bool Utils::enable_privilege(const wchar_t* privilege_name) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilege_name, &luid)) {
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL res = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hToken);

    return res && (err == ERROR_SUCCESS);
}

bool Utils::enable_required_privileges() {
    bool ok = true;
    const wchar_t* privileges[] = {
        SE_PROF_SINGLE_PROCESS_NAME,     // SeProfileSingleProcessPrivilege
        SE_INCREASE_QUOTA_NAME,          // SeIncreaseQuotaPrivilege
        SE_DEBUG_NAME,                   // SeDebugPrivilege
        SE_SHUTDOWN_NAME,                // SeShutdownPrivilege
        SE_SYSTEM_PROFILE_NAME           // SeSystemProfilePrivilege
    };

    for (const auto* priv : privileges) {
        if (!enable_privilege(priv)) {
            // Note: on unprivileged non-admin runs, some privileges may not be in token
            ok = false;
        }
    }
    return ok;
}

bool Utils::is_running_as_admin() {
    BOOL is_admin = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&nt_auth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &is_admin);
        FreeSid(admin_group);
    }
    return is_admin != FALSE;
}

bool Utils::is_running_as_system() {
    BOOL is_system = FALSE;
    PSID system_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&nt_auth, 1, SECURITY_LOCAL_SYSTEM_RID, 0,
                                  0, 0, 0, 0, 0, 0, &system_group)) {
        CheckTokenMembership(nullptr, system_group, &is_system);
        FreeSid(system_group);
    }
    return is_system != FALSE;
}

std::wstring Utils::utf8_to_wide(const std::string& utf8_str) {
    if (utf8_str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, utf8_str.data(), static_cast<int>(utf8_str.size()), nullptr, 0);
    if (size_needed <= 0) return std::wstring();

    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.data(), static_cast<int>(utf8_str.size()), &result[0], size_needed);
    return result;
}

std::string Utils::wide_to_utf8(const std::wstring& wide_str) {
    if (wide_str.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wide_str.data(), static_cast<int>(wide_str.size()), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return std::string();

    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide_str.data(), static_cast<int>(wide_str.size()), &result[0], size_needed, nullptr, nullptr);
    return result;
}

std::wstring Utils::guid_to_wstring(const GUID& guid) {
    wchar_t buf[64] = {0};
    swprintf_s(buf, 64, L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return std::wstring(buf);
}

std::wstring Utils::get_executable_path() {
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (len == buffer.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize(buffer.size() * 2);
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    return std::wstring(buffer.data(), len);
}

std::wstring Utils::get_executable_dir() {
    std::wstring path = get_executable_path();
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(0, pos);
    }
    return path;
}

std::wstring Utils::get_win32_error_message(DWORD error_code) {
    if (error_code == 0) return L"The operation completed successfully.";

    LPWSTR msg_buf = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&msg_buf), 0, nullptr);

    if (size == 0 || !msg_buf) {
        return L"Unknown Win32 Error: " + std::to_wstring(error_code);
    }

    std::wstring result(msg_buf, size);
    LocalFree(msg_buf);

    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

std::wstring Utils::get_last_error_message() {
    return get_win32_error_message(GetLastError());
}

uint64_t Utils::get_current_time_millis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()
    );
}

double Utils::get_high_resolution_timestamp_sec() {
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter)) {
        return static_cast<double>(get_current_time_millis()) / 1000.0;
    }
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
}

std::wstring Utils::get_process_name_by_pid(DWORD pid) {
    if (pid == 0) return L"System Idle Process";
    if (pid == 4) return L"System";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        return L"";
    }

    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD size = static_cast<DWORD>(buffer.size());
    if (QueryFullProcessImageNameW(hProcess, 0, buffer.data(), &size)) {
        CloseHandle(hProcess);
        std::wstring full_path(buffer.data(), size);
        auto pos = full_path.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            return full_path.substr(pos + 1);
        }
        return full_path;
    }

    CloseHandle(hProcess);
    return L"";
}

} // namespace surface_optimizer
