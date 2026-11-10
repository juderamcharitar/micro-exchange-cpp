# Usage: powershell -ExecutionPolicy Bypass -File .\start_server_windows.ps1 -Port 8765
param([ValidateRange(0,65535)][int]$Port = 8765, [string]$Journal = ".\data\commands.journal", [switch]$TcpNoDelay, [ValidateSet("sync", "group")][string]$JournalMode = "sync")
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$exe = '.\build\Release\exchange_server.exe'
if (-not (Test-Path $exe)) { $exe = '.\build\exchange_server.exe' }
if (-not (Test-Path $exe)) {
    Write-Error 'Server executable not found. First run run_windows.ps1 from x64 Native Tools Command Prompt for VS 2022.'
    exit 1
}
Write-Host "Starting localhost exchange server on port $Port, journal=$Journal (Ctrl+C to stop)"
if ($TcpNoDelay) {
    & $exe --port $Port --journal $Journal --journal-mode $JournalMode --tcp-nodelay
} else {
    & $exe --port $Port --journal $Journal --journal-mode $JournalMode
}
exit $LASTEXITCODE
