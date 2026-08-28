#include <windows.h>
#include <powrprof.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>

#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "user32.lib")

static const GUID GUID_SUBGROUP_PROCESSOR = 
    { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
static const GUID GUID_EPP_POLICY = 
    { 0x36687f9e, 0xe3a5, 0x4dbf, { 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63 } };

bool ForceSetForeground(HWND hwnd) {
    DWORD currentThreadId = GetCurrentThreadId();
    HWND hCurrent = GetForegroundWindow();
    DWORD foregroundThreadId = hCurrent ? GetWindowThreadProcessId(hCurrent, NULL) : 0;
    DWORD targetThreadId = GetWindowThreadProcessId(hwnd, NULL);

    if (foregroundThreadId != currentThreadId && foregroundThreadId != 0) {
        AttachThreadInput(currentThreadId, foregroundThreadId, TRUE);
    }
    if (targetThreadId != currentThreadId && targetThreadId != 0) {
        AttachThreadInput(currentThreadId, targetThreadId, TRUE);
    }

    AllowSetForegroundWindow(ASFW_ANY);

    keybd_event(VK_MENU, 0, 0, 0);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);

    if (foregroundThreadId != currentThreadId && foregroundThreadId != 0) {
        AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
    }
    if (targetThreadId != currentThreadId && targetThreadId != 0) {
        AttachThreadInput(currentThreadId, targetThreadId, FALSE);
    }

    return (GetForegroundWindow() == hwnd);
}

uint32_t ReadAcEpp(const GUID& schemeGuid) {
    DWORD val = 999;
    GUID s = schemeGuid;
    GUID sub = GUID_SUBGROUP_PROCESSOR;
    GUID epp = GUID_EPP_POLICY;
    PowerReadACValueIndex(NULL, &s, &sub, &epp, &val);
    return val;
}

int main() {
    GUID* pActiveScheme = nullptr;
    if (PowerGetActiveScheme(NULL, &pActiveScheme) != ERROR_SUCCESS || !pActiveScheme) {
        std::cerr << "Failed to get active scheme\n";
        return 1;
    }
    GUID activeScheme = *pActiveScheme;
    LocalFree(pActiveScheme);

    std::cout << "[LATENCY_TEST] Starting Ramp Latency Benchmark...\n";
    std::cout << "[LATENCY_TEST] Initial EPP: " << ReadAcEpp(activeScheme) << "%\n";

    // Find Target Window A & B
    HWND hwndA = FindWindowA("SynthBurstAppClass", "RampTargetWindowA");
    HWND hwndB = FindWindowA("SynthBurstAppClass", "RampTargetWindowB");

    if (!hwndA || !hwndB) {
        std::cerr << "Could not find both RampTargetWindowA and RampTargetWindowB. Make sure they are running.\n";
        return 2;
    }

    std::cout << "[LATENCY_TEST] Found HWND_A=" << (uintptr_t)hwndA << ", HWND_B=" << (uintptr_t)hwndB << "\n";

    // First focus Window A
    ForceSetForeground(hwndA);
    std::cout << "[LATENCY_TEST] Focused Window A. Waiting 6s for housekeeping decay to idle (60%)...\n";
    std::this_thread::sleep_for(std::chrono::seconds(6));

    uint32_t preEpp = ReadAcEpp(activeScheme);
    std::cout << "[LATENCY_TEST] Pre-switch EPP: " << preEpp << "%\n";

    if (preEpp != 60) {
        std::cout << "[LATENCY_TEST] Warning: Pre-switch EPP is not 60% (is " << preEpp << "%)\n";
    }

    // Now switch to Window B and measure exact transition latency
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    bool focused = ForceSetForeground(hwndB);

    uint32_t currentEpp = preEpp;
    bool rampDetected = false;
    double elapsedMs = 0.0;

    for (int i = 0; i < 200; ++i) {
        currentEpp = ReadAcEpp(activeScheme);
        if (currentEpp == 0) {
            QueryPerformanceCounter(&end);
            elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
            rampDetected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    std::cout << "[LATENCY_TEST] SetForeground focused=" << (focused ? "TRUE" : "FALSE") << "\n";
    std::cout << "[LATENCY_TEST] Result: RampDetected=" << (rampDetected ? "TRUE" : "FALSE")
              << ", LatencyMs=" << elapsedMs 
              << ", FinalEPP=" << currentEpp << "%\n";

    if (rampDetected && elapsedMs < 100.0) {
        std::cout << "[LATENCY_TEST] PASS: Latency (" << elapsedMs << "ms) is under 100ms budget!\n";
        return 0;
    } else {
        std::cout << "[LATENCY_TEST] FAIL: Latency (" << elapsedMs << "ms) exceeded 100ms or not detected.\n";
        return 3;
    }
}
