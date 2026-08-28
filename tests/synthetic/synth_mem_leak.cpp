// ============================================================================
// synth_mem_leak.cpp — Synthetic Controlled Monotonic Memory Allocation Workload
// Purpose: Simulates memory leaks to test runaway leak detector and WS management
// ============================================================================

#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>
#include <cstring>
#include <atomic>

static std::atomic<bool> g_running{true};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    int chunk_mb = 10;
    int interval_ms = 500;
    int max_mb = 300;
    int hold_sec = 10; // Time to hold after reaching max_mb
    int duration_sec = 0;
    bool silent = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--chunk-mb" && i + 1 < argc) {
            chunk_mb = std::atoi(argv[++i]);
        } else if (arg == "--interval-ms" && i + 1 < argc) {
            interval_ms = std::atoi(argv[++i]);
        } else if (arg == "--max-mb" && i + 1 < argc) {
            max_mb = std::atoi(argv[++i]);
        } else if (arg == "--hold-sec" && i + 1 < argc) {
            hold_sec = std::atoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--silent") {
            silent = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: synth_mem_leak.exe [--chunk-mb N] [--interval-ms MS] [--max-mb N] [--hold-sec SEC] [--duration SEC] [--silent]\n";
            return 0;
        }
    }

    if (chunk_mb < 1) chunk_mb = 1;
    if (interval_ms < 10) interval_ms = 10;
    if (max_mb < 10) max_mb = 10;

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    DWORD pid = GetCurrentProcessId();
    std::cout << "[SYNTH_MEM_LEAK_READY] PID=" << pid 
              << " ChunkMB=" << chunk_mb 
              << " IntervalMs=" << interval_ms 
              << " MaxMB=" << max_mb 
              << " HoldSec=" << hold_sec << std::endl;
    std::cout.flush();

    std::vector<char*> allocated_blocks;
    const size_t chunk_bytes = static_cast<size_t>(chunk_mb) * 1024 * 1024;
    size_t total_allocated_mb = 0;

    auto start_time = std::chrono::steady_clock::now();

    while (g_running) {
        if (total_allocated_mb < static_cast<size_t>(max_mb)) {
            // Allocate and commit pages
            char* block = static_cast<char*>(VirtualAlloc(NULL, chunk_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (block) {
                // Touch every 4KB page to force physical memory backing
                for (size_t offset = 0; offset < chunk_bytes; offset += 4096) {
                    block[offset] = static_cast<char>(offset & 0xFF);
                }
                allocated_blocks.push_back(block);
                total_allocated_mb += chunk_mb;

                PROCESS_MEMORY_COUNTERS_EX pmc{};
                GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

                if (!silent) {
                    std::cout << "[SYNTH_MEM_LEAK_STEP] PID=" << pid 
                              << " AllocatedMB=" << total_allocated_mb 
                              << " WS=" << (pmc.WorkingSetSize / 1024) << "KB" 
                              << " Private=" << (pmc.PrivateUsage / 1024) << "KB" << std::endl;
                    std::cout.flush();
                }
            } else {
                std::cerr << "[SYNTH_MEM_LEAK_WARN] VirtualAlloc failed at " << total_allocated_mb << " MB\n";
                break;
            }
        } else {
            // Reached max MB, hold in memory
            if (!silent) {
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
                std::cout << "[SYNTH_MEM_LEAK_HOLD] PID=" << pid 
                          << " AllocatedMB=" << total_allocated_mb 
                          << " WS=" << (pmc.WorkingSetSize / 1024) << "KB" << std::endl;
                std::cout.flush();
            }
            if (hold_sec > 0) {
                for (int s = 0; s < hold_sec && g_running; ++s) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                break;
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
        if (duration_sec > 0 && elapsed >= duration_sec) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    // Cleanup
    for (char* b : allocated_blocks) {
        if (b) VirtualFree(b, 0, MEM_RELEASE);
    }
    allocated_blocks.clear();

    std::cout << "[SYNTH_MEM_LEAK_EXIT] PID=" << pid << " PeakAllocatedMB=" << total_allocated_mb << std::endl;
    return 0;
}
