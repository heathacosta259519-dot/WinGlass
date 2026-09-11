$ErrorActionPreference = 'Stop'
$vs = 'G:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vs)) { throw "Visual Studio toolchain not found: $vs" }
$root = $PSScriptRoot
$source = Join-Path $root 'winglass.cpp'
$output = Join-Path $root 'winglass.exe'
cmd /c "`"$vs`" && cl /nologo /std:c++17 /EHsc /W4 `"$source`" /link /SUBSYSTEM:WINDOWS /OUT:`"$output`" user32.lib gdi32.lib dwmapi.lib psapi.lib"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$editorSource = Join-Path $root 'winglass-config.cpp'
$editorOutput = Join-Path $root 'winglass-config.exe'
cmd /c "`"$vs`" && cl /nologo /std:c++17 /EHsc /W4 `"$editorSource`" /link /SUBSYSTEM:WINDOWS /OUT:`"$editorOutput`" user32.lib gdi32.lib shell32.lib comctl32.lib"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$watchdogSource = Join-Path $root 'winglass-watchdog.cpp'
$watchdogOutput = Join-Path $root 'winglass-watchdog.exe'
cmd /c "`"$vs`" && cl /nologo /std:c++17 /EHsc /W4 `"$watchdogSource`" /link /SUBSYSTEM:WINDOWS /OUT:`"$watchdogOutput`" user32.lib shell32.lib"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output 'Built winglass.exe, winglass-config.exe, and winglass-watchdog.exe (x64).'
