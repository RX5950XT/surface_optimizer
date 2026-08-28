# Surface Pro 7 Optimizer - Service Uninstallation Script
# Requires Administrator Privileges

$ErrorActionPreference = 'Stop'

# Self-elevate if not running as Administrator
if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Elevating privileges to Administrator..." -ForegroundColor Yellow
    Start-Process powershell.exe "-NoProfile -ExecutionPolicy Bypass -File `\"$PSCommandPath`\"" -Verb RunAs
    exit
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Split-Path -Parent $scriptDir
$exePath = Join-Path $rootDir "surface_optimizer.exe"

if (!(Test-Path $exePath)) {
    Write-Error "surface_optimizer.exe not found at: $exePath."
}

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host " Uninstalling Surface Pro 7 Optimizer Windows Service    " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

& $exePath --uninstall

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nService successfully removed." -ForegroundColor Green
    & $exePath --status
} else {
    Write-Error "Service uninstallation failed with exit code: $LASTEXITCODE"
}