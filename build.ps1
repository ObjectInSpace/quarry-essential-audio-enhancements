# Builds build\X3DAudio1_7.dll (the mod) and the offline tests.
#   .\build.ps1          build
#   .\build.ps1 -Test    build and run the tests
# Needs a MinGW-w64 GCC on PATH (e.g. WinLibs), or set $env:QSA_GCC.
param([switch]$Test)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$gcc = if ($env:QSA_GCC) { $env:QSA_GCC } else { 'gcc' }
New-Item -ItemType Directory -Force "$root\build" | Out-Null

& $gcc -shared -O2 -Wall -Wextra -static -s -o "$root\build\X3DAudio1_7.dll" `
    "$root\src\qsa.c" "$root\src\qsa.def" -lole32
if ($LASTEXITCODE) { throw "mod build failed" }

if ($Test) {
    & $gcc -O2 -Wall -static -o "$root\build\test_qsa.exe" "$root\tests\test_qsa.c" `
        "$root\tests\fake_wwise.c" "$root\tests\test_qsa.def" -lole32
    if ($LASTEXITCODE) { throw "test build failed" }
    & $gcc -shared -O2 -Wall -static -o "$root\build\fake_windows.dll" "$root\tests\fake_windows.c" -lole32
    if ($LASTEXITCODE) { throw "helper build failed" }
    $failed = 0
    Push-Location $root
    foreach ($mode in 'surround', 'surround51', 'surround51refused', 'headphones', 'spatial') {
        & "$root\build\test_qsa.exe" "$root\build\X3DAudio1_7.dll" $mode
        if ($LASTEXITCODE) { $failed++ }
    }
    Pop-Location
    Remove-Item "$root\build\QuarrySpatial.ini", "$root\build\QuarrySpatial.log" -ErrorAction SilentlyContinue
    if ($failed) { throw "$failed test run(s) failed" }
    Write-Host "all tests passed"
}
