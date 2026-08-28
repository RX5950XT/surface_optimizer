$objdump = "C:\msys64\ucrt64\bin\objdump.exe"
$exe = "C:\RX5950XT\terminal\surface_optimizer\surface_optimizer.exe"

$lines = & $objdump -p $exe
Write-Host "=== MATCHING IMPORTED WIN32 SYMBOLS ==="
$keywords = @("Service", "Mutex", "Token", "Wait", "Power", "Event", "Hook", "Privilege", "Security")
foreach ($line in $lines) {
    foreach ($kw in $keywords) {
        if ($line.IndexOf($kw, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            Write-Host $line.Trim()
            break
        }
    }
}
