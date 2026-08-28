#include "core/logger.hpp"
#include "core/utils.hpp"
#include <windows.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>

namespace surface_optimizer {

Logger& Logger::get_instance() {
    static Logger instance;
    return instance;
}

Logger::Logger() = default;

Logger::~Logger() {
    shutdown();
}

LogLevel Logger::parse_level(const std::wstring& level_str) {
    if (level_str == L"DEBUG" || level_str == L"debug") return LogLevel::Debug;
    if (level_str == L"INFO" || level_str == L"info") return LogLevel::Info;
    if (level_str == L"WARN" || level_str == L"warn") return LogLevel::Warn;
    if (level_str == L"ERROR" || level_str == L"error") return LogLevel::Error;
    if (level_str == L"NONE" || level_str == L"none") return LogLevel::None;
    return LogLevel::Info;
}

void Logger::initialize(LogLevel level, const std::wstring& log_file_path, bool log_to_console, bool log_to_file) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
    m_log_file_path = log_file_path;
    m_log_to_console = log_to_console;
    m_log_to_file = log_to_file;

    if (m_file_stream.is_open()) {
        m_file_stream.close();
    }

    if (m_log_to_file && !m_log_file_path.empty()) {
        try {
            std::filesystem::path p(m_log_file_path);
            if (p.has_parent_path()) {
                std::filesystem::create_directories(p.parent_path());
            }
            m_file_stream.open(m_log_file_path.c_str(), std::ios::out | std::ios::app);
        } catch (...) {
            // Failed to open log file, fallback to console
        }
    }

    m_initialized = true;
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
}

LogLevel Logger::get_level() const {
    return m_level;
}

std::wstring Logger::format_prefix(LogLevel level, const char* file, int line) {
    auto now = std::chrono::system_clock::now();
    auto timer = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &timer);
#else
    localtime_r(&timer, &tm_buf);
#endif

    const wchar_t* level_str = L"INFO ";
    switch (level) {
        case LogLevel::Debug: level_str = L"DEBUG"; break;
        case LogLevel::Info:  level_str = L"INFO "; break;
        case LogLevel::Warn:  level_str = L"WARN "; break;
        case LogLevel::Error: level_str = L"ERROR"; break;
        default: break;
    }

    std::wstringstream ss;
    ss << L"[" << std::put_time(&tm_buf, L"%Y-%m-%d %H:%M:%S")
       << L"." << std::setfill(L'0') << std::setw(3) << ms.count()
       << L"] [" << level_str << L"] [TID:" << GetCurrentThreadId() << L"]";

    if (file && m_level == LogLevel::Debug) {
        std::string filename(file);
        auto pos = filename.find_last_of("\\/");
        if (pos != std::string::npos) {
            filename = filename.substr(pos + 1);
        }
        ss << L" [" << Utils::utf8_to_wide(filename) << L":" << line << L"]";
    }

    ss << L" ";
    return ss.str();
}

void Logger::write_console(LogLevel level, const std::wstring& prefix, const std::wstring& message) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hConsole || hConsole == INVALID_HANDLE_VALUE) {
        std::wcout << prefix << message << std::endl;
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(hConsole, &csbi);
    WORD default_attr = csbi.wAttributes ? csbi.wAttributes : (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    std::wcout << prefix;

    WORD color_attr = default_attr;
    switch (level) {
        case LogLevel::Debug:
            color_attr = FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN;
            break;
        case LogLevel::Info:
            color_attr = FOREGROUND_INTENSITY | FOREGROUND_GREEN;
            break;
        case LogLevel::Warn:
            color_attr = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN;
            break;
        case LogLevel::Error:
            color_attr = FOREGROUND_INTENSITY | FOREGROUND_RED;
            break;
        default:
            break;
    }

    SetConsoleTextAttribute(hConsole, color_attr);
    std::wcout << message << std::endl;
    SetConsoleTextAttribute(hConsole, default_attr);
}

void Logger::write_file(const std::wstring& prefix, const std::wstring& message) {
    if (m_file_stream.is_open()) {
        m_file_stream << prefix << message << std::endl;
    }
}

void Logger::log(LogLevel level, const char* file, int line, const std::wstring& message) {
    if (level < m_level || level == LogLevel::None) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    std::wstring prefix = format_prefix(level, file, line);

    if (m_log_to_console) {
        write_console(level, prefix, message);
    }

    if (m_log_to_file) {
        write_file(prefix, message);
    }
}

void Logger::log(LogLevel level, const char* file, int line, const std::string& message) {
    log(level, file, line, Utils::utf8_to_wide(message));
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file_stream.is_open()) {
        m_file_stream.flush();
    }
    std::wcout.flush();
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file_stream.is_open()) {
        m_file_stream.flush();
        m_file_stream.close();
    }
    m_initialized = false;
}

} // namespace surface_optimizer
