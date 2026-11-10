# Run from Windows PowerShell after installing CMake and a C++20 compiler.
# Example: powershell -ExecutionPolicy Bypass -File .\run_windows.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

cmake -S . -B build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exe = '.\build\Release\exchange_cli.exe'
$bench = '.\build\Release\exchange_bench.exe'
if (-not (Test-Path $exe)) { $exe = '.\build\exchange_cli.exe' }
if (-not (Test-Path $bench)) { $bench = '.\build\exchange_bench.exe' }

Write-Host "`n=== Deterministic exchange demo ==="
& $exe --script .\examples\demo_orders.txt
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "`n=== Synthetic in-process benchmark ==="
& $bench 100000
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "`n=== Existing v0.8 TCP gateway and WAL research ==="
Write-Host 'In a separate terminal, run:'
Write-Host 'powershell -ExecutionPolicy Bypass -File .\start_server_windows.ps1'
Write-Host 'Then run: py -3 .\client\exchange_client.py --script .\examples\network_orders.txt'
Write-Host 'For the preserved v0.8 fault/retry integration test:'
Write-Host 'ctest --test-dir build -C Release -R exchange_fault_idempotency_integration --output-on-failure'
Write-Host 'For book depth vs matching costs:'
Write-Host '.\build\Release\exchange_depth_bench.exe --levels 10,100,1000 --cycles 3000 --repetitions 3 --csv .\benchmarks\depth_windows.csv'
Write-Host 'For isolated concurrent clients + WAL off/strict-sync/group (launches temporary servers automatically):'
Write-Host 'py -3 .\client\bench_scaling.py --pairs 1,2,4,8 --rounds 75 --repeats 3 --mode all --tcp-nodelay --json-output .\benchmarks\scaling_windows_v08.json'
Write-Host 'py -3 .\bench\report_v07.py --scaling .\benchmarks\scaling_windows_v08.json --output .\benchmarks\WINDOWS_V080_SCALING_REPORT.md'
Write-Host "`n=== v1.0 synthetic flow generator + deterministic replay ==="
Write-Host 'For deterministic synthetic mixed-regime research (regular PowerShell, after building):'
Write-Host '.\build\Release\exchange_flow_sim.exe --generate .\benchmarks\flow_windows.csv --seed 42 --events 12000 --profile mixed --summary .\benchmarks\flow_windows.json --trades .\benchmarks\flow_windows_trades.csv'
Write-Host '.\build\Release\exchange_flow_sim.exe --replay .\benchmarks\flow_windows.csv --expect-summary .\benchmarks\flow_windows.json --summary .\benchmarks\flow_windows_replayed.json'
Write-Host 'py -3 .\sim\report_flow.py --summaries .\benchmarks\flow_windows.json --output .\benchmarks\WINDOWS_V090_FLOW_REPORT.md'
exit 0
