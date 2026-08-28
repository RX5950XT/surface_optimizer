#include <windows.h>
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include "optimizer/memory_manager.hpp"
#include "core/config.hpp"
#include "core/utils.hpp"

using namespace surface_optimizer;

int wmain() {
    std::wcout << L"=====================================================\n";
    std::wcout << L" FORENSIC VERIFICATION HARNESS FOR MEMORY MANAGER M3 \n";
    std::wcout << L"=====================================================\n";

    auto& mm = MemoryManager::get_instance();
    bool init_ok = mm.initialize();
    std::wcout << L"[CHECK 1] MemoryManager::initialize() returned: " << (init_ok ? L"PASS (true)" : L"FAIL (false)") << L"\n";
    assert(init_ok);

    // 1. Check get_memory_stats()
    auto stats = mm.get_memory_stats();
    std::wcout << L"[CHECK 2] MemoryStats: Load=" << stats.memory_load_percent 
               << L"%, Total=" << (stats.total_phys_bytes / 1024 / 1024) 
               << L" MB, Avail=" << (stats.avail_phys_bytes / 1024 / 1024) << L" MB\n";
    assert(stats.total_phys_bytes > 0);
    assert(stats.avail_phys_bytes > 0);
    assert(stats.memory_load_percent > 0 && stats.memory_load_percent <= 100);

    // 2. Check System PID and own PID protection
    uint32_t own_pid = GetCurrentProcessId();
    bool own_protected = mm.is_pid_protected(own_pid);
    bool pid0_protected = mm.is_pid_protected(0);
    bool pid4_protected = mm.is_pid_protected(4);
    std::wcout << L"[CHECK 3] PID Protection: Own PID(" << own_pid << L")=" << own_protected 
               << L", PID 0=" << pid0_protected 
               << L", PID 4=" << pid4_protected << L"\n";
    assert(own_protected);
    assert(pid0_protected);
    assert(pid4_protected);

    // 3. Check Foreground PID Protection & Grace Period
    uint32_t dummy_fg_pid = 99991;
    mm.on_foreground_process_changed(dummy_fg_pid, L"dummy_app.exe");
    bool fg_protected = mm.is_pid_protected(dummy_fg_pid);
    std::wcout << L"[CHECK 4] Foreground PID (" << dummy_fg_pid << L") protection: " << (fg_protected ? L"PASS" : L"FAIL") << L"\n";
    assert(fg_protected);

    // Change foreground to another PID and check that dummy_fg_pid is STILL protected within 10s grace period
    uint32_t new_fg_pid = 99992;
    mm.on_foreground_process_changed(new_fg_pid, L"new_app.exe");
    bool recent_protected = mm.is_pid_protected(dummy_fg_pid);
    std::wcout << L"[CHECK 5] Grace Period Protection (10s sliding window) for PID " << dummy_fg_pid << L": " << (recent_protected ? L"PASS" : L"FAIL") << L"\n";
    assert(recent_protected);

    // Set grace period to 50ms and wait 100ms, then check that it expires
    mm.set_recent_focus_grace_period_ms(50);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // Trigger cleanup
    mm.on_foreground_process_changed(new_fg_pid, L"new_app.exe");
    bool expired_protected = mm.is_pid_protected(dummy_fg_pid);
    std::wcout << L"[CHECK 6] Grace Period Expiration after timeout: " << (!expired_protected ? L"PASS (Expired as expected)" : L"FAIL (Still protected)") << L"\n";
    assert(!expired_protected);

    // Restore grace period
    mm.set_recent_focus_grace_period_ms(10000);

    // 4. Check Standby list purge
    bool standby_ok = mm.purge_standby_list();
    std::wcout << L"[CHECK 7] purge_standby_list() execution: " << (standby_ok ? L"PASS (Command succeeded)" : L"FAIL") << L"\n";
    assert(standby_ok);

    // 5. Check optimize_memory with foreground PID
    auto trim_res = mm.optimize_memory(new_fg_pid, true);
    std::wcout << L"[CHECK 8] optimize_memory() executed: " 
               << trim_res.processes_trimmed << L" processes trimmed, "
               << (trim_res.bytes_freed / 1024 / 1024) << L" MB freed, Standby purged="
               << (trim_res.standby_list_purged ? L"YES" : L"NO") << L"\n";
    assert(trim_res.processes_trimmed > 0);
    assert(trim_res.standby_list_purged == true);

    mm.shutdown();
    std::wcout << L"ALL FORENSIC CHECKS PASSED EMPIRICALLY!\n";
    return 0;
}
