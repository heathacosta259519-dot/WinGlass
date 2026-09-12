$ErrorActionPreference = 'Stop'
$vs = 'G:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vs)) { throw "Visual Studio toolchain not found: $vs" }

$root = $PSScriptRoot
$resourceScript = Join-Path $root 'winglass.rc'

# Every build product - .res, .obj and .exe - lands in release\, so the source
# tree never accumulates build output. release\ is gitignored.
$build = Join-Path $root 'release'
New-Item -ItemType Directory -Force -Path $build | Out-Null

# Compile one executable. The resource script is recompiled per target with a
# bare /d flag (no value, so there is nothing for cmd to quote or mangle), which
# makes winglass.rc emit that binary's own FileDescription and OriginalFilename.
# The version numbers themselves come from winglass-resource.h and are shared.
function Build-Target {
    param(
        [Parameter(Mandatory)] [string]   $Name,          # base name, e.g. 'winglass'
        [Parameter(Mandatory)] [string]   $TargetDefine,  # e.g. 'WINGLASS_TARGET_MAIN'
        [Parameter(Mandatory)] [string[]] $Libs
    )

    $source = Join-Path $root  ($Name + '.cpp')
    $res    = Join-Path $build ($Name + '.res')
    $obj    = Join-Path $build ($Name + '.obj')
    $exe    = Join-Path $build ($Name + '.exe')

    $rcArgs = 'rc /nologo /fo "{0}" /d {1} "{2}"' -f $res, $TargetDefine, $resourceScript
    $clArgs = 'cl /nologo /std:c++17 /EHsc /W4 /Fo"{0}" "{1}" "{2}" /link /SUBSYSTEM:WINDOWS /OUT:"{3}" {4}' -f $obj, $source, $res, $exe, ($Libs -join ' ')

    cmd /c "`"$vs`" && cd /d `"$root`" && $rcArgs && $clArgs"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Build-Target -Name 'winglass' -TargetDefine 'WINGLASS_TARGET_MAIN' -Libs @('user32.lib', 'gdi32.lib', 'dwmapi.lib', 'psapi.lib')

# The editor needs the Windows Imaging Component (PNG clipboard decoding) and
# ole32 for COM; neither is required by the resident process or the watchdog.
Build-Target -Name 'winglass-config' -TargetDefine 'WINGLASS_TARGET_EDITOR' -Libs @('user32.lib', 'gdi32.lib', 'shell32.lib', 'comctl32.lib', 'windowscodecs.lib', 'ole32.lib')

Build-Target -Name 'winglass-watchdog' -TargetDefine 'WINGLASS_TARGET_WATCHDOG' -Libs @('user32.lib', 'shell32.lib')

# winglass.exe resolves its configuration relative to its own directory, not
# the current one, so a bare release folder cannot start. Ship the example
# configuration and the license next to the binaries, so the folder is a
# complete and license-compliant distribution.
foreach ($extra in 'config.example.yaml', 'config.example.ini', 'LICENSE') {
    Copy-Item (Join-Path $root $extra) (Join-Path $build $extra) -Force
}

# The distribution gets its own short readme, not the repository one: the
# repository README documents building from source and its relative paths do
# not apply to somebody who just unzipped a release.
Copy-Item (Join-Path $root 'README.release.txt') (Join-Path $build 'README.txt') -Force

Write-Output "Built winglass.exe, winglass-config.exe and winglass-watchdog.exe into $build (x64, application icon + version info)."
