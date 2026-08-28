# test_m3_stress.ps1 - Empirical M3 Memory Manager & Zero-Stutter Challenger Harness

$ErrorActionPreference = "Continue"

$NativeWin32Type = Add-Type -MemberDefinition @"
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("psapi.dll", SetLastError = true)]
    public static extern bool GetProcessMemoryInfo(IntPtr hProcess, out PROCESS_MEMORY_COUNTERS_EX counters, uint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_MEMORY_COUNTERS_EX {
        public uint cb;
        public uint PageFaultCount;
        public UIntPtr PeakWorkingSetSize;
        public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage;
        public UIntPtr QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage;
        public UIntPtr QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage;
        public UIntPtr PeakPagefileUsage;
        public UIntPtr PrivateUsage;
    }
"@ -Name "NativeWin32Stress" -Namespace "ChallengerM3" -PassThru

function Get-ProcessWS([int]$pidToQuery) {
    $h = [ChallengerM3.NativeWin32Stress]::OpenProcess(0x0410, $false, [uint32]$pidToQuery)
    if ($h -ne [IntPtr]::Zero) {
        $pmc = New-Object ChallengerM3.NativeWin32Stress+PROCESS_MEMORY_COUNTERS_EX
        $pmc.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($pmc)
        if ([ChallengerM3.NativeWin32Stress]::GetProcessMemoryInfo($h, [ref]$pmc, $pmc.cb)) {
            [ChallengerM3.NativeWin32Stress]::CloseHandle($h) | Out-Null
            return [uint64]$pmc.WorkingSetSize.ToUInt64()
        }
        [ChallengerM3.NativeWin32Stress]::CloseHandle($h) | Out-Null
    }
    return 0
}

function Wait-ForWorkingSetCommit([int]$pidToQuery, [uint64]$targetMinBytes, [int]$timeoutMs = 5000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $ws = Get-ProcessWS $pidToQuery
        if ($ws -ge $targetMinBytes) {
            return $ws
        }
        Start-Sleep -Milliseconds 100
    }
    return (Get-ProcessWS $pidToQuery)
}

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host " CHALLENGER M3 EMPIRICAL STRESS TEST SUITE" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

$results = @{}

# -----------------------------------------------------------------------------
# TEST 1: synth_idle_bloat.exe Trimming (100MB, 200MB, 300MB)
# -----------------------------------------------------------------------------
Write-Host "`n--- [TEST 1] synth_idle_bloat.exe Memory Trimming (Target: >= 30% reduction) ---" -ForegroundColor Yellow

$bloatSizes = @(100, 200, 300)
$bloatResults = @()

foreach ($sizeMB in $bloatSizes) {
    Write-Host "`nTesting Idle Bloat size: $sizeMB MB..." -ForegroundColor White
    $bloatProc = Start-Process -FilePath "C:\RX5950XT\terminal\surface_optimizer\tests\synthetic\synth_idle_bloat.exe" -ArgumentList "--size-mb $sizeMB --duration 30 --silent" -PassThru
    
    # Wait for memory to be committed into resident working set
    $targetBytes = [uint64]($sizeMB * 1024 * 1024 * 0.7)
    $wsBefore = Wait-ForWorkingSetCommit $bloatProc.Id $targetBytes 5000
    $wsBeforeMB = [math]::Round($wsBefore / 1MB, 2)
    Write-Host "  PID: $($bloatProc.Id), Initial Working Set: $wsBeforeMB MB"

    # Execute --trim-memory
    $trimOut = & "C:\RX5950XT\terminal\surface_optimizer\surface_optimizer.exe" --trim-memory 2>&1 | Out-String
    Start-Sleep -Milliseconds 500

    $wsAfter = Get-ProcessWS $bloatProc.Id
    $wsAfterMB = [math]::Round($wsAfter / 1MB, 2)
    $reductionPct = if ($wsBefore -gt 0) { [math]::Round((1.0 - ($wsAfter / $wsBefore)) * 100.0, 2) } else { 0.0 }

    $passed = ($reductionPct -ge 30.0)
    $statusColor = if ($passed) { "Green" } else { "Red" }
    Write-Host "  Post-Trim Working Set: $wsAfterMB MB"
    Write-Host "  Working Set Reduction: $reductionPct% (Target: >= 30%) -> $(if ($passed) {'PASS'} else {'FAIL'})" -ForegroundColor $statusColor

    $bloatResults += [PSCustomObject]@{
        SizeMB       = $sizeMB
        PID          = $bloatProc.Id
        WS_Before_MB = $wsBeforeMB
        WS_After_MB  = $wsAfterMB
        ReductionPct = $reductionPct
        Pass         = $passed
    }

    if (-not $bloatProc.HasExited) {
        Stop-Process -Id $bloatProc.Id -Force -ErrorAction SilentlyContinue
    }
}
$results["Test1_BloatTrim"] = $bloatResults

# -----------------------------------------------------------------------------
# TEST 2: Foreground Zero-Stutter Guard on synth_burst_app.exe
# -----------------------------------------------------------------------------
Write-Host "`n--- [TEST 2] synth_burst_app.exe Foreground Zero-Stutter Guard ---" -ForegroundColor Yellow

$burstProc = Start-Process -FilePath "C:\RX5950XT\terminal\surface_optimizer\tests\synthetic\synth_burst_app.exe" -ArgumentList "--burst-ms 100 --auto-burst 1 --duration 30 --title ChallengerBurstApp" -PassThru
Start-Sleep -Seconds 2

# Find window and bring to foreground
$hwnd = $burstProc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
    # Search for window by title
    $hwnd = (Get-Process -Id $burstProc.Id).MainWindowHandle
}
if ($hwnd -ne [IntPtr]::Zero) {
    [ChallengerM3.NativeWin32Stress]::ShowWindow($hwnd, 9) | Out-Null # SW_RESTORE
    [ChallengerM3.NativeWin32Stress]::BringWindowToTop($hwnd) | Out-Null
    [ChallengerM3.NativeWin32Stress]::SetForegroundWindow($hwnd) | Out-Null
}
Start-Sleep -Milliseconds 500

$fgHwnd = [ChallengerM3.NativeWin32Stress]::GetForegroundWindow()
$fgPid = 0
[ChallengerM3.NativeWin32Stress]::GetWindowThreadProcessId($fgHwnd, [ref]$fgPid)
Write-Host "  Burst App PID: $($burstProc.Id), Current Foreground PID: $fgPid"

$burstWsBefore = Get-ProcessWS $burstProc.Id
$burstWsBeforeMB = [math]::Round($burstWsBefore / 1MB, 2)
Write-Host "  Burst App Initial WS: $burstWsBeforeMB MB"

# Run trim-memory
$trimOut2 = & "C:\RX5950XT\terminal\surface_optimizer\surface_optimizer.exe" --trim-memory 2>&1 | Out-String
Start-Sleep -Milliseconds 500

$burstWsAfter = Get-ProcessWS $burstProc.Id
$burstWsAfterMB = [math]::Round($burstWsAfter / 1MB, 2)
$burstReductionPct = if ($burstWsBefore -gt 0) { [math]::Round((1.0 - ($burstWsAfter / $burstWsBefore)) * 100.0, 2) } else { 0.0 }

Write-Host "  Burst App Post-Trim WS: $burstWsAfterMB MB"
Write-Host "  Burst App WS Reduction: $burstReductionPct%"

# Zero-stutter guard asserts foreground app should NOT be trimmed
$guardPassed = ($burstReductionPct -le 5.0)
$guardColor = if ($guardPassed) { "Green" } else { "Red" }
Write-Host "  Zero-Stutter Guard Evaluation: $(if ($guardPassed) {'PASS (Foreground Protected)'} else {'FAIL (Foreground Was Trimmed!)'})" -ForegroundColor $guardColor

$results["Test2_ZeroStutter"] = [PSCustomObject]@{
    BurstPID          = $burstProc.Id
    ForegroundPID     = $fgPid
    WS_Before_MB      = $burstWsBeforeMB
    WS_After_MB       = $burstWsAfterMB
    ReductionPct      = $burstReductionPct
    GuardPassed       = $guardPassed
}

if (-not $burstProc.HasExited) {
    Stop-Process -Id $burstProc.Id -Force -ErrorAction SilentlyContinue
}

# -----------------------------------------------------------------------------
# TEST 3: Daemon Memory Footprint & Idle CPU Overhead
# -----------------------------------------------------------------------------
Write-Host "`n--- [TEST 3] Daemon Resource Overhead (< 10MB RAM, < 0.1% CPU) ---" -ForegroundColor Yellow

$daemonProc = Start-Process -FilePath "C:\RX5950XT\terminal\surface_optimizer\surface_optimizer.exe" -ArgumentList "--interactive" -PassThru
Start-Sleep -Seconds 2

$daemonPid = $daemonProc.Id
Write-Host "  Daemon Running in Interactive Mode (PID: $daemonPid)"

# Sample RAM and CPU over 5 seconds
$ramSamples = @()
$cpuSamples = @()

$lastCpuTime = $daemonProc.TotalProcessorTime.TotalMilliseconds
$lastSampleTime = [DateTime]::UtcNow

for ($i = 0; $i -lt 5; $i++) {
    Start-Sleep -Seconds 1
    $daemonProc.Refresh()
    $wsBytes = Get-ProcessWS $daemonPid
    $wsMB = [math]::Round($wsBytes / 1MB, 2)
    $ramSamples += $wsMB

    $now = [DateTime]::UtcNow
    $curCpuTime = $daemonProc.TotalProcessorTime.TotalMilliseconds
    $elapsedMs = ($now - $lastSampleTime).TotalMilliseconds
    $cpuUsagePct = if ($elapsedMs -gt 0) { [math]::Round((($curCpuTime - $lastCpuTime) / ($elapsedMs * [Environment]::ProcessorCount)) * 100.0, 4) } else { 0.0 }

    $lastCpuTime = $curCpuTime
    $lastSampleTime = $now
    $cpuSamples += $cpuUsagePct
    $sampleIdx = $i + 1
    Write-Host "    Sample $sampleIdx`: RAM = $wsMB MB, CPU = $cpuUsagePct%"
}

$maxRAM = ($ramSamples | Measure-Object -Maximum).Maximum
$avgRAM = [math]::Round(($ramSamples | Measure-Object -Average).Average, 2)
$avgCPU = [math]::Round(($cpuSamples | Measure-Object -Average).Average, 4)

$ramPassed = ($maxRAM -lt 10.0)
$cpuPassed = ($avgCPU -lt 0.1)

Write-Host "  Max Daemon RAM: $maxRAM MB (Limit: < 10 MB) -> $(if ($ramPassed) {'PASS'} else {'FAIL'})" -ForegroundColor $(if ($ramPassed) {'Green'} else {'Red'})
Write-Host "  Avg Daemon CPU: $avgCPU% (Limit: < 0.1%) -> $(if ($cpuPassed) {'PASS'} else {'FAIL'})" -ForegroundColor $(if ($cpuPassed) {'Green'} else {'Red'})

$results["Test3_DaemonOverhead"] = [PSCustomObject]@{
    DaemonPID = $daemonPid
    MaxRAM_MB = $maxRAM
    AvgRAM_MB = $avgRAM
    AvgCPU_Pct = $avgCPU
    RAM_Pass  = $ramPassed
    CPU_Pass  = $cpuPassed
}

# Stop Daemon
if (-not $daemonProc.HasExited) {
    Stop-Process -Id $daemonProc.Id -Force -ErrorAction SilentlyContinue
}

Write-Host "`n=================================================================" -ForegroundColor Cyan
Write-Host " SUMMARY OF EMPIRICAL RESULTS" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
$results | ConvertTo-Json -Depth 4 | Write-Host
