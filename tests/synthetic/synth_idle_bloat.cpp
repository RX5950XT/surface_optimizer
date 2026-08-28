// ============================================================================
// synth_idle_bloat.cpp — Synthetic Idle Working Set Bloat Workload
// Purpose: Allocates 100~300MB resident working set, verifies allocation, then
//          idles to allow E2E verification of EmptyWorkingSet trimming (>30%).
// ============================================================================

#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>
#include <atomic>

static std::atomic<bool> g_running{true};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

uint64_t get_current_working_set_kb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return pmc.WorkingSetSize / 1024;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    int size_mb = 200;
    int duration_sec = 0;
    int report_interval_ms = 1000;
    bool silent = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--size-mb" && i + 1 < argc) {
            size_mb = std::atoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--report-interval-ms" && i + 1 < argc) {
            report_interval_ms = std::atoi(argv[++i]);
        } else if (arg == "--silent") {
            silent = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: synth_idle_bloat.exe [--size-mb MB] [--duration SEC] [--report-interval-ms MS] [--silent]\n";
            return 0;
        }
    }

    if (size_mb < 10) size_mb = 10;
    if (report_interval_ms < 100) report_interval_ms = 100;

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    DWORD pid = GetCurrentProcessId();

    // Allocate memory block
    size_t total_bytes = static_cast<size_t>(size_mb) * 1024 * 1024;
    char* memory_block = static_cast<char*>(VirtualAlloc(NULL, total_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory_block) {
        std::cerr << "[SYNTH_IDLE_BLOAT_ERROR] Failed to allocate " << size_mb << " MB\n";
        return 1;
    }

    // Touch all pages to guarantee residency in physical RAM Working Set
    for (size_t offset = 0; offset < total_bytes; offset += 4096) {
        memory_block[offset] = static_cast<char>((offset / 4096) & 0xFF);
    }

    uint64_t initial_ws_kb = get_current_working_set_kb();

    std::cout << "[SYNTH_IDLE_BLOAT_READY] PID=" << pid 
              << " TargetMB=" << size_mb 
              << " InitialWS_KB=" << initial_ws_kb << std::endl;
    std::cout.flush();

    auto start_time = std::chrono::steady_clock::now();
    uint64_t min_observed_ws_kb = initial_ws_kb;
    bool trim_logged = false;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(report_interval_ms));

        uint64_t cur_ws_kb = get_current_working_set_kb();
        if (cur_ws_kb < min_observed_ws_kb) {
            min_observed_ws_kb = cur_ws_kb;
        }

        double reduction_pct = 0.0;
        if (initial_ws_kb > 0 && cur_ws_kb < initial_ws_kb) {
            reduction_pct = (1.0 - (static_cast<double>(cur_ws_kb) / initial_ws_kb)) * 100.0;
        }

        if (reduction_pct >= 30.0 && !trim_logged) {
            std::cout << "[SYNTH_IDLE_BLOAT_TRIM_DETECTED] PID=" << pid 
                      << " InitialWS_KB=" << initial_ws_kb 
                      << " CurrentWS_KB=" << cur_ws_kb 
                      << " ReductionPct=" << reduction_pct << "%" << std::endl;
            std::cout.flush();
            trim_logged = true;
        }

        if (!silent) {
            std::cout << "[SYNTH_IDLE_BLOAT_HEARTBEAT] PID=" << pid 
                      << " WS_KB=" << cur_ws_kb 
                      << " ReductionPct=" << reduction_pct << "%" << std::endl;
            std::cout.flush();
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
        if (duration_sec > 0 && elapsed >= duration_sec) {
            break;
        }
    }

    // Cleanup
    if (memory_block) {
        VirtualFree(memory_block, 0, MEM_RELEASE);
    }

    std::cout << "[SYNTH_IDLE_BLOAT_EXIT] PID=" << pid 
              << " FinalMinWS_KB=" << min_observed_ws_kb << std::endl;
    return 0;
}
