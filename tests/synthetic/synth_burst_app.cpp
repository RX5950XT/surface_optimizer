// ============================================================================
// synth_burst_app.cpp — Focus-Triggered Burst Workload
// Purpose: Simulates interactive apps that trigger CPU bursts upon gaining focus,
//          used to verify <100ms instant EPP / frequency ramp response.
// ============================================================================

#include <windows.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdlib>

static std::atomic<bool> g_running{true};
static int g_burst_ms = 200;
static HWND g_hwnd = NULL;
static LARGE_INTEGER g_freq;

void execute_burst(int duration_ms, const char* trigger_source) {
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    DWORD pid = GetCurrentProcessId();
    std::cout << "[SYNTH_BURST_START] PID=" << pid 
              << " Source=" << trigger_source 
              << " TargetMs=" << duration_ms 
              << " QpcStart=" << start.QuadPart << std::endl;
    std::cout.flush();

    // High CPU burst calculation
    auto t_start = std::chrono::steady_clock::now();
    volatile uint64_t sum = 0;
    while (true) {
        for (int i = 0; i < 10000; ++i) {
            sum += (i * 31337) ^ (sum << 3);
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t_start).count() >= duration_ms) {
            break;
        }
    }

    QueryPerformanceCounter(&end);
    double elapsed_us = static_cast<double>(end.QuadPart - start.QuadPart) * 1000000.0 / g_freq.QuadPart;

    std::cout << "[SYNTH_BURST_DONE] PID=" << pid 
              << " ElapsedUs=" << static_cast<uint64_t>(elapsed_us) 
              << " Result=" << sum << std::endl;
    std::cout.flush();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SETFOCUS:
    case WM_ACTIVATE:
        if (msg == WM_SETFOCUS || (msg == WM_ACTIVATE && (LOWORD(wParam) != WA_INACTIVE))) {
            std::thread([]() {
                execute_burst(g_burst_ms, "FOCUS");
            }).detach();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        g_running = false;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int main(int argc, char* argv[]) {
    int duration_sec = 0;
    int auto_burst_interval_sec = 0;
    int focus_after_sec = 0;
    std::string title = "SurfaceOptimizer_SynthBurstApp";
    bool headless_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--burst-ms" && i + 1 < argc) {
            g_burst_ms = std::atoi(argv[++i]);
        } else if (arg == "--auto-burst" && i + 1 < argc) {
            auto_burst_interval_sec = std::atoi(argv[++i]);
        } else if (arg == "--focus-after" && i + 1 < argc) {
            focus_after_sec = std::atoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--title" && i + 1 < argc) {
            title = argv[++i];
        } else if (arg == "--headless") {
            headless_mode = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: synth_burst_app.exe [--burst-ms MS] [--auto-burst SEC] [--focus-after SEC] [--duration SEC] [--title NAME] [--headless]\n";
            return 0;
        }
    }

    QueryPerformanceFrequency(&g_freq);
    DWORD pid = GetCurrentProcessId();

    if (!headless_mode) {
        // Register and create standard Win32 Window
        HINSTANCE hInstance = GetModuleHandle(NULL);
        WNDCLASSA wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = "SynthBurstAppClass";
        RegisterClassA(&wc);

        g_hwnd = CreateWindowExA(
            0, "SynthBurstAppClass", title.c_str(),
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            300, 200, NULL, NULL, hInstance, NULL
        );

        if (g_hwnd) {
            ShowWindow(g_hwnd, SW_SHOW);
            UpdateWindow(g_hwnd);
        }
    }

    std::cout << "[SYNTH_BURST_READY] PID=" << pid 
              << " HWND=" << reinterpret_cast<uintptr_t>(g_hwnd) 
              << " BurstMs=" << g_burst_ms 
              << " AutoBurstSec=" << auto_burst_interval_sec 
              << " FocusAfterSec=" << focus_after_sec << std::endl;
    std::cout.flush();

    // Focus-after thread if enabled
    std::thread focus_thread;
    if (focus_after_sec > 0 && g_hwnd != NULL) {
        focus_thread = std::thread([focus_after_sec]() {
            std::this_thread::sleep_for(std::chrono::seconds(focus_after_sec));
            if (!g_running) return;
            AllowSetForegroundWindow(ASFW_ANY);
            keybd_event(VK_MENU, 0, 0, 0);
            SetForegroundWindow(g_hwnd);
            BringWindowToTop(g_hwnd);
            keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
            std::cout << "[SYNTH_BURST_FOCUSED] Self-focused HWND=" << reinterpret_cast<uintptr_t>(g_hwnd) << std::endl;
            std::cout.flush();
        });
    }

    // Auto-burst thread if enabled
    std::thread auto_burst_thread;
    if (auto_burst_interval_sec > 0) {
        auto_burst_thread = std::thread([auto_burst_interval_sec]() {
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::seconds(auto_burst_interval_sec));
                if (!g_running) break;
                execute_burst(g_burst_ms, "AUTO_TIMER");
            }
        });
    }

    auto start_time = std::chrono::steady_clock::now();

    // Message loop
    MSG msg;
    while (g_running) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
        if (duration_sec > 0 && elapsed >= duration_sec) {
            g_running = false;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (focus_thread.joinable()) {
        focus_thread.join();
    }
    if (auto_burst_thread.joinable()) {
        auto_burst_thread.join();
    }

    std::cout << "[SYNTH_BURST_EXIT] PID=" << pid << std::endl;
    return 0;
}
