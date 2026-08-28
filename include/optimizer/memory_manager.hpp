#pragma once

#include <windows.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <cstdint>

namespace surface_optimizer {

struct MemoryStats {
    uint32_t memory_load_percent = 0;
    uint64_t total_phys_bytes = 0;
    uint64_t avail_phys_bytes = 0;
};

struct TrimResult {
    size_t processes_trimmed = 0;
    uint64_t bytes_freed = 0;
    bool standby_list_purged = false;
};

enum class SystemMemoryListCommand : int {
    MemoryEmptyWorkingSets = 0,
    MemoryFlushModifiedList = 1,
    MemoryPurgeStandbyList = 2,
    MemoryPurgeLowPriorityStandbyList = 3,
    MemoryCommandMax = 4
};

class MemoryManager {
public:
    static MemoryManager& get_instance();

    virtual bool initialize();
    virtual MemoryStats get_memory_stats() const;
    virtual TrimResult optimize_memory(uint32_t current_foreground_pid, bool force_standby_purge);
    virtual bool trim_process_working_set(uint32_t pid);
    virtual bool purge_standby_list();
    virtual void shutdown();

    // Event & Housekeeping Integration
    virtual void on_foreground_process_changed(uint32_t pid, const std::wstring& image_name);
    virtual void on_housekeeping(uint32_t current_foreground_pid, bool is_ac);

    // Protection & Inspection
    virtual bool is_pid_protected(uint32_t pid) const;
    virtual void set_recent_focus_grace_period_ms(uint32_t ms);
    virtual uint32_t get_current_foreground_pid() const;
    virtual TrimResult get_last_trim_result() const;
    virtual uint64_t get_last_trim_timestamp() const;

    virtual ~MemoryManager() = default;

private:
    MemoryManager();
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    typedef LONG NTSTATUS;
    typedef NTSTATUS (NTAPI *pfnNtSetSystemInformation)(INT SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength);

    mutable std::mutex m_mutex;
    pfnNtSetSystemInformation m_nt_set_system_information = nullptr;
    bool m_initialized = false;

    // Foreground tracking and zero-stutter guard
    uint32_t m_current_foreground_pid = 0;
    uint32_t m_grace_period_ms = 10000; // 10 seconds grace period
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> m_recent_foreground_history;

    // Adaptive trimming state & cooldown
    std::chrono::steady_clock::time_point m_last_trim_time{};
    TrimResult m_last_trim_result{};
    uint64_t m_last_trim_epoch_ms = 0;

    void cleanup_expired_focus_history_locked();
    bool is_image_allowlisted(const std::wstring& image_name) const;
    bool is_pid_protected_locked(uint32_t pid) const;
    bool purge_standby_list_internal();
};

} // namespace surface_optimizer
