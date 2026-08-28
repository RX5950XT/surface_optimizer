// ============================================================================
// synth_cpu_hog.cpp — Synthetic Background High-CPU Workload
// Purpose: Simulates runaway background CPU-intensive processes for E2E testing
// ============================================================================

#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <cstdlib>

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_total_iterations{0};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// Compute-intensive kernel: pseudo-random hashing & prime checking
void cpu_worker(int thread_id, int target_pct) {
    uint64_t state = 0x123456789ABCDEF0ULL ^ (static_cast<uint64_t>(thread_id) * 0x9E3779B97F4A7C15ULL);
    
    auto last_duty_check = std::chrono::steady_clock::now();
    uint64_t iter = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        // Run a tight compute batch
        for (int i = 0; i < 50000; ++i) {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            state *= 0x2545F4914F6CDD1DULL;
        }
        iter += 50000;

        // Duty-cycle control if target_pct < 100
        if (target_pct < 100) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_duty_check).count();
            if (elapsed_us >= 10000) { // Check every 10ms
                int64_t sleep_us = (elapsed_us * (100 - target_pct)) / target_pct;
                if (sleep_us > 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                }
                last_duty_check = std::chrono::steady_clock::now();
            }
        }
    }
    g_total_iterations += iter;
}

int main(int argc, char* argv[]) {
    int num_threads = 1;
    int duration_sec = 0; // 0 = indefinite
    int target_pct = 100;
    bool silent = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            num_threads = std::atoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--target-pct" && i + 1 < argc) {
            target_pct = std::atoi(argv[++i]);
        } else if (arg == "--silent") {
            silent = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: synth_cpu_hog.exe [--threads N] [--duration SEC] [--target-pct 1-100] [--silent]\n";
            return 0;
        }
    }

    if (num_threads < 1) num_threads = 1;
    if (target_pct < 1) target_pct = 1;
    if (target_pct > 100) target_pct = 100;

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    DWORD pid = GetCurrentProcessId();
    std::cout << "[SYNTH_CPU_HOG_READY] PID=" << pid 
              << " Threads=" << num_threads 
              << " TargetPct=" << target_pct 
              << " DurationSec=" << duration_sec << std::endl;
    std::cout.flush();

    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back(cpu_worker, t, target_pct);
    }

    int seconds_elapsed = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        seconds_elapsed++;

        if (!silent && seconds_elapsed % 5 == 0) {
            PROCESS_MEMORY_COUNTERS pmc{};
            GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
            std::cout << "[SYNTH_CPU_HOG_BEAT] PID=" << pid 
                      << " Elapsed=" << seconds_elapsed << "s" 
                      << " WS=" << (pmc.WorkingSetSize / 1024) << "KB" << std::endl;
            std::cout.flush();
        }

        if (duration_sec > 0 && seconds_elapsed >= duration_sec) {
            g_running = false;
            break;
        }
    }

    g_running = false;
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    std::cout << "[SYNTH_CPU_HOG_EXIT] PID=" << pid << " TotalElapsed=" << seconds_elapsed << "s" << std::endl;
    return 0;
}
