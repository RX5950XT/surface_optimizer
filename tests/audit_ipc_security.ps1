$source = Get-Content -Raw -Encoding utf8 "$PSScriptRoot\..\src\core\service.cpp"

if ($source -match 'SetSecurityDescriptorDacl') {
    throw 'IPC must not use a null DACL.'
}
if ($source -notmatch 'D:P\(A;;GA;;;SY\)\(A;;GRGW;;;IU\)\(A;;0x00100000;;;IU\)') {
    throw 'IPC DACL must grant SYSTEM and interactive users only.'
}
if ($source -notmatch 'cmd_value > 1') {
    throw 'UI command values must be validated.'
}

Write-Host 'IPC security audit passed.'
