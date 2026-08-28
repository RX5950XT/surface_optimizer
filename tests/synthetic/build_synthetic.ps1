# ============================================================================
# build_synthetic.ps1 — Build Synthetic Workloads via PowerShell & GCC
# ============================================================================

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "[BUILD_SYNTHETIC] Building synthetic workloads in $ScriptDir..." -ForegroundColor Cyan

$targets = @(
    @{ Source = "synth_cpu_hog.cpp"; Output = "synth_cpu_hog.exe"; Extra = @("-lpsapi", "-ladvapi32", "-lkernel32") },
    @{ Source = "synth_mem_leak.cpp"; Output = "synth_mem_leak.exe"; Extra = @("-lpsapi", "-ladvapi32", "-lkernel32") },
    @{ Source = "synth_burst_app.cpp"; Output = "synth_burst_app.exe"; Extra = @("-luser32", "-lgdi32", "-lkernel32") },
    @{ Source = "synth_idle_bloat.cpp"; Output = "synth_idle_bloat.exe"; Extra = @("-lpsapi", "-lkernel32") }
)

foreach ($t in $targets) {
    $src = Join-Path $ScriptDir $t.Source
    $out = Join-Path $ScriptDir $t.Output
    Write-Host "  Compiling $($t.Source) -> $($t.Output)..." -NoNewline
    
    $cmd = @("g++", "-std=c++20", "-O3", "-s", "-static", "-Wall", "-Wextra", "-o", $out, $src) + $t.Extra
    $proc = Start-Process -FilePath "g++" -ArgumentList ($cmd[1..($cmd.Length - 1)]) -NoNewWindow -Wait -PassThru

    if ($proc.ExitCode -eq 0 -and (Test-Path $out)) {
        $size = (Get-Item $out).Length
        Write-Host " OK ($([math]::Round($size/1024, 1)) KB)" -ForegroundColor Green
    } else {
        Write-Host " FAILED (ExitCode=$($proc.ExitCode))" -ForegroundColor Red
        exit 1
    }
}

Write-Host "[BUILD_SYNTHETIC] All 4 synthetic workloads compiled successfully!" -ForegroundColor Green
