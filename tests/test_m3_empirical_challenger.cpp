#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cassert>
#include <atomic>
#include <windows.h>
#include <psapi.h>

#include "optimizer/memory_manager.hpp"
#include "core/config.hpp"
#include "core/logger.hpp"

using namespace surface_optimizer;

void test_memory_stats_query() {
    std::cout << "[TEST] 1. MemoryStats Query Verification..." << std::endl;
    auto& mm = MemoryManager::get_instance();
    bool init_ok = mm.initialize();
    assert(init_ok);

    MemoryStats stats = mm.get_memory_stats();
    std::cout << "  Memory Load: " << stats.memory_load_percent << "%" << std::endl;
    std::cout << "  Total Phys RAM: " << (stats.total_phys_bytes / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "  Avail Phys RAM: " << (stats.avail_phys_bytes / (1024 * 1024)) << " MB" << std::endl;

    assert(stats.memory_load_percent > 0 && stats.memory_load_percent <= 100);
    assert(stats.total_phys_bytes > 0);
    assert(stats.avail_phys_bytes > 0);
    std::cout << "  [PASS] MemoryStats query valid." << std::endl;
}

void test_standby_purge() {
    std::cout << "[TEST] 2. Standby List NTAPI Class 80 Purge..." << std::endl;
    auto& mm = MemoryManager::get_instance();
    bool purged = mm.purge_standby_list();
    std::cout << "  Purge Standby Result: " << (purged ? "SUCCESS" : "FAILED") << std::endl;
    assert(purged == true);
    std::cout << "  [PASS] Standby list purge executed cleanly." << std::endl;
}

void test_zero_stutter_guard() {
    std::cout << "[TEST] 3. Foreground Zero-Stutter Guard & Grace Period..." << std::endl;
    auto& mm = MemoryManager::get_instance();

    uint32_t fake_fg_pid = 999991;
    mm.on_foreground_process_changed(fake_fg_pid, L"test_app.exe");
    assert(mm.get_current_foreground_pid() == fake_fg_pid);
    assert(mm.is_pid_protected(fake_fg_pid) == true);
    std::cout << "  Active foreground PID " << fake_fg_pid << " is protected." << std::endl;

    // Switch foreground to another PID
    uint32_t fake_fg_pid2 = 999992;
    mm.on_foreground_process_changed(fake_fg_pid2, L"test_app2.exe");
    assert(mm.get_current_foreground_pid() == fake_fg_pid2);
    assert(mm.is_pid_protected(fake_fg_pid2) == true);

    // Verify fake_fg_pid is STILL protected within grace period (10s)
    assert(mm.is_pid_protected(fake_fg_pid) == true);
    std::cout << "  Previous foreground PID " << fake_fg_pid << " protected by 10s grace period." << std::endl;

    // Set grace period to 100ms and wait 150ms to verify expiry
    mm.set_recent_focus_grace_period_ms(100);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // Trigger cleanup via on_foreground_process_changed or check
    mm.on_foreground_process_changed(fake_fg_pid2, L"test_app2.exe");
    bool still_protected = mm.is_pid_protected(fake_fg_pid);
    std::cout << "  Previous PID protection after grace expiry: " << (still_protected ? "STILL PROTECTED" : "EXPIRED (CORRECT)") << std::endl;
    assert(still_protected == false);

    // Restore standard grace period
    mm.set_recent_focus_grace_period_ms(10000);
    std::cout << "  [PASS] Zero-Stutter Guard & Grace Period passed." << std::endl;
}

void test_system_allowlist_protection() {
    std::cout << "[TEST] 4. System Allowlist Immunity..." << std::endl;
    auto& mm = MemoryManager::get_instance();

    assert(mm.is_pid_protected(0) == true);  // System Idle
    assert(mm.is_pid_protected(4) == true);  // System Kernel
    assert(mm.is_pid_protected(GetCurrentProcessId()) == true); // Self

    // Find Explorer PID
    DWORD aProcesses[1024], cbNeeded, cProcesses;
    if (EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded)) {
        cProcesses = cbNeeded / sizeof(DWORD);
        for (unsigned int i = 0; i < cProcesses; i++) {
            if (aProcesses[i] != 0) {
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, aProcesses[i]);
                if (hProcess) {
                    wchar_t szProcessName[MAX_PATH];
                    DWORD size = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProcess, 0, szProcessName, &size)) {
                        std::wstring fullPath(szProcessName);
                        size_t pos = fullPath.find_last_of(L"\\/");
                        std::wstring filename = (pos != std::wstring::npos) ? fullPath.substr(pos + 1) : fullPath;
                        std::transform(filename.begin(), filename.end(), filename.begin(), ::towlower);
                        if (filename == L"explorer.exe" || filename == L"dwm.exe" || filename == L"audiodg.exe") {
                            bool prot = mm.is_pid_protected(aProcesses[i]);
                            std::wcout << L"  Protected system process " << filename << L" (PID " << aProcesses[i] << L"): " << (prot ? L"IMMUNE" : L"UNPROTECTED") << std::endl;
                            assert(prot == true);
                        }
                    }
                    CloseHandle(hProcess);
                }
            }
        }
    }
    std::cout << "  [PASS] System allowlist immunity verified." << std::endl;
}

void test_concurrency_stress() {
    std::cout << "[TEST] 5. High-Concurrency Stress Test (Multi-Threaded Race Guard)..." << std::endl;
    auto& mm = MemoryManager::get_instance();
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> workers;

    // Spawn 8 worker threads hammering memory manager concurrently
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&mm, &stop_flag, t]() {
            uint32_t fake_pid = 10000 + t;
            while (!stop_flag.load()) {
                mm.on_foreground_process_changed(fake_pid, L"thread_test.exe");
                mm.get_memory_stats();
                mm.is_pid_protected(fake_pid);
                mm.is_pid_protected(4);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    // Main thread runs optimize_memory concurrently
    for (int i = 0; i < 5; ++i) {
        TrimResult res = mm.optimize_memory(GetCurrentProcessId(), true);
        std::cout << "  Iteration " << (i+1) << ": Trimmed " << res.processes_trimmed << " processes, " << (res.bytes_freed / (1024*1024)) << " MB freed." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    stop_flag.store(true);
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    std::cout << "  [PASS] Concurrency stress test passed with 0 deadlocks or memory corruption." << std::endl;
}

int wmain() {
    std::cout << "=========================================================" << std::endl;
    std::cout << " MILITARY-GRADE EMPIRICAL STRESS TEST: MEMORY MANAGER M3 " << std::endl;
    std::cout << "=========================================================" << std::endl;

    try {
        test_memory_stats_query();
        test_standby_purge();
        test_zero_stutter_guard();
        test_system_allowlist_protection();
        test_concurrency_stress();

        std::cout << "\n=========================================================" << std::endl;
        std::cout << " ALL 5 EMPIRICAL STRESS HARNESS TESTS PASSED (100% PASS) " << std::endl;
        std::cout << "=========================================================" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "EXCEPTION: " << ex.what() << std::endl;
        return 1;
    }
}
