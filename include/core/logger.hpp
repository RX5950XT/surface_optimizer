#pragma once

#include <string>
#include <string_view>
#include <mutex>
#include <fstream>
#include <cstdint>

namespace surface_optimizer {

enum class LogLevel : uint8_t {
    Debug = 0,
    Info,
    Warn,
    Error,
    None
};

class Logger {
public:
    static Logger& get_instance();

    void initialize(LogLevel level, const std::wstring& log_file_path, bool log_to_console, bool log_to_file);
    void set_level(LogLevel level);
    LogLevel get_level() const;
    static LogLevel parse_level(const std::wstring& level_str);

    void log(LogLevel level, const char* file, int line, const std::wstring& message);
    void log(LogLevel level, const char* file, int line, const std::string& message);

    void flush();
    void shutdown();

private:
    Logger();
    ~Logger();

    std::wstring format_prefix(LogLevel level, const char* file, int line);
    void write_console(LogLevel level, const std::wstring& prefix, const std::wstring& message);
    void write_file(const std::wstring& prefix, const std::wstring& message);

    LogLevel m_level = LogLevel::Info;
    std::wstring m_log_file_path;
    bool m_log_to_console = true;
    bool m_log_to_file = false;
    std::mutex m_mutex;
    std::wofstream m_file_stream;
    bool m_initialized = false;
};

} // namespace surface_optimizer

#define LOG_DEBUG(msg) ::surface_optimizer::Logger::get_instance().log(::surface_optimizer::LogLevel::Debug, __FILE__, __LINE__, msg)
#define LOG_INFO(msg)  ::surface_optimizer::Logger::get_instance().log(::surface_optimizer::LogLevel::Info,  __FILE__, __LINE__, msg)
#define LOG_WARN(msg)  ::surface_optimizer::Logger::get_instance().log(::surface_optimizer::LogLevel::Warn,  __FILE__, __LINE__, msg)
#define LOG_ERROR(msg) ::surface_optimizer::Logger::get_instance().log(::surface_optimizer::LogLevel::Error, __FILE__, __LINE__, msg)
