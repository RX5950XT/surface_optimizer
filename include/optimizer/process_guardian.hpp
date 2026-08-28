#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <cstdint>

namespace surface_optimizer {

struct ProcessSnapshot {
    uint32_t pid = 0;
    std::wstring image_name;
    uint64_t kernel_time = 0;   // 100-ns ticks
    uint64_t user_time = 0;     // 100-ns ticks
    uint64_t working_set_bytes = 0;
    std::chrono::steady_clock::time_point sample_time{};
};

struct ThrottleRecord {
    uint32_t pid = 0;
    std::wstring image_name;
    std::chrono::steady_clock::time_point throttled_at{};
    bool eco_qos_applied = false;
    bool priority_demoted = false;
    double last_cpu_percent = 0.0;
    uint64_t last_ws_bytes = 0;
};

struct GuardianStats {
    size_t processes_scanned = 0;
    size_t cpu_hogs_detected = 0;
    size_t mem_leaks_detected = 0;
    size_t throttled_count = 0;
    size_t skipped_allowlist = 0;
    size_t skipped_foreground = 0;
};

class ProcessGuardian {
public:
    static ProcessGuardian& get_instance();

    virtual bool initialize();
    virtual void shutdown();

    // Core housekeeping entry point (called from Service event loop)
    virtual GuardianStats on_housekeeping(uint32_t current_foreground_pid);

    // Inspection
    virtual GuardianStats get_last_stats() const;
    virtual std::vector<ThrottleRecord> get_active_throttles() const;
    virtual bool is_process_allowlisted(const std::wstring& image_name) const;

    // Foreground tracking (to protect active app)
    virtual void on_foreground_process_changed(uint32_t pid, const std::wstring& image_name);

    virtual ~ProcessGuardian() = default;

private:
    ProcessGuardian();
    ProcessGuardian(const ProcessGuardian&) = delete;
    ProcessGuardian& operator=(const ProcessGuardian&) = delete;

    // Snapshot & analysis
    std::vector<ProcessSnapshot> take_snapshot() const;
    double compute_cpu_percent(const ProcessSnapshot& prev, const ProcessSnapshot& curr) const;
    int64_t compute_ws_delta(const ProcessSnapshot& prev, const ProcessSnapshot& curr) const;

    // Throttling actions
    bool apply_eco_qos(uint32_t pid);
    bool demote_priority(uint32_t pid);
    void cleanup_stale_throttles();

    // Protection checks
    bool is_protected(uint32_t pid, const std::wstring& image_name) const;

    mutable std::mutex m_mutex;
    bool m_initialized = false;

    // Previous snapshot for delta computation
    std::unordered_map<uint32_t, ProcessSnapshot> m_prev_snapshots;

    // Sustained anomaly counters (pid -> consecutive anomaly tick count)
    std::unordered_map<uint32_t, uint32_t> m_cpu_hog_ticks;
    std::unordered_map<uint32_t, uint32_t> m_mem_leak_ticks;

    // Active throttle records
    std::unordered_map<uint32_t, ThrottleRecord> m_throttled_pids;

    // Foreground PID
    uint32_t m_current_foreground_pid = 0;

    // Last stats
    GuardianStats m_last_stats{};
};

} // namespace surface_optimizer
