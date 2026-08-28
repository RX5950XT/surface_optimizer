# Surface Pro 7 Optimizer - Native Build Script
# Configures MSYS2 UCRT64 GCC 14.2.0 toolchain and compiles surface_optimizer.exe

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

Write-Host '=======================================================' -ForegroundColor Cyan
Write-Host ' Building Surface Pro 7 Native Optimizer (Modern C++20)  ' -ForegroundColor Cyan
Write-Host '=======================================================' -ForegroundColor Cyan

# Configure Toolchain Paths
$env:PATH = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH

$gccPath = 'C:\msys64\ucrt64\bin\g++.exe'
$makePath = 'C:\msys64\ucrt64\bin\mingw32-make.exe'

if (!(Test-Path $gccPath)) {
    Write-Error "GCC compiler not found at: $gccPath"
}

Write-Host 'Compiling with GNU GCC 14.2.0 (UCRT64)...' -ForegroundColor Yellow
& $makePath clean
& $makePath -j4

if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed with exit code $LAPTEXITCODE"
}
	$exePath = Join-Path $scriptDir 'surface_optimizer.exe'
if (Test-Path $exePath) {
    $exeSize = (Get-Item $exePath).Length / 1KB
    Write-Host 'Build Succeeded!' -ForegroundColor Green
    Write-Host "Output Binary: $exePath ($([Math]::Round($exeSize, 2)) KB)" -ForegroundColor Green
    
    Write-Host "`nVerifying Import Dependencies (Zero-DLL Requirement)..." -ForegroundColor Yellow
    $objdump = 'C:\msys64\ucrt64\bin\objdump.exe'
    if (Test-Path $objdump) {
        $dlls = & $objdump -p $exePath | Select-String 'DLL Name:'
        $dlls | ForEach-Object { Write-Host "  $_" -ForegroundColor Gray }
    }
} else {
    Write-Error "Target executable was not produced: $exePath"
}