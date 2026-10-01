# Builds build\X3DAudio1_7.dll (the mod) and the offline tests.
#   .\build.ps1          build
#   .\build.ps1 -Test    build and run the tests
# Needs a MinGW-w64 GCC on PATH (e.g. WinLibs), or set $env:QSA_GCC.
param([switch]$Test)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$gcc = if ($env:QSA_GCC) { $env:QSA_GCC } else { 'gcc' }
New-Item -ItemType Directory -Force "$root\build" | Out-Null
$mh = "$root\third_party\minhook"
$minhook = @("$mh\src\hook.c", "$mh\src\buffer.c", "$mh\src\trampoline.c", "$mh\src\hde\hde64.c")

& $gcc -shared -O2 -Wall -Wextra -static -s -o "$root\build\X3DAudio1_7.dll" `
    "$root\src\qsa.c" $minhook -I "$mh\include" "$root\src\qsa.def" -lole32
if ($LASTEXITCODE) { throw "mod build failed" }

if ($Test) {
    & $gcc -O2 -Wall -static -o "$root\build\test_qsa.exe" "$root\tests\test_qsa.c" `
        "$root\tests\fake_wwise.c" "$root\tests\test_qsa.def" -lole32
    if ($LASTEXITCODE) { throw "test build failed" }
    & $gcc -shared -O2 -Wall -static -o "$root\build\fake_windows.dll" "$root\tests\fake_windows.c" -lole32
    if ($LASTEXITCODE) { throw "helper build failed" }
    $failed = 0
    Push-Location $root
    Remove-Item "$root\build\dialogue_patched.bnk" -ErrorAction SilentlyContinue
    foreach ($mode in 'auto', 'layout51', 'layout51no', 'mono', 'headphones', 'spatial', 'device', 'nodevice', 'nocomp',
                      'dialogue', 'dialoguegame', 'meter', 'meterbad', 'prevlog') {
        & "$root\build\test_qsa.exe" "$root\build\X3DAudio1_7.dll" $mode
        if ($LASTEXITCODE) { $failed++ }
    }
    # With the game's own dialogue bank (QSA_SPEECH_BNK; see tests\test_qsa.c), check the
    # changed copy independently. QSA_WWISER, if set, names wwiser.pyz for one more check.
    if ($env:QSA_SPEECH_BNK -and (Test-Path "$root\build\dialogue_patched.bnk")) {
        $py = @("$root\tools\check_dialogue_bank.py", $env:QSA_SPEECH_BNK, "$root\build\dialogue_patched.bnk")
        if ($env:QSA_WWISER) { $py += $env:QSA_WWISER }
        python @py
        if ($LASTEXITCODE) { $failed++ }
    }
    Pop-Location
    Remove-Item "$root\build\QuarryEssentialAudio.ini", "$root\build\QuarryEssentialAudio.log", "$root\build\QuarryEssentialAudio_meter.log", "$root\build\QuarryEssentialAudio_meter_summary.txt", "$root\build\*.prev" -ErrorAction SilentlyContinue
    if ($failed) { throw "$failed test run(s) failed" }
    Write-Host "all tests passed"
}
