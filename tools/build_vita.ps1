# Port-only (checkpoint 132vt1, research/VITA.md "Build"): configures and builds the PS Vita targets with the
# Windows-native VitaSDK (no WSL): omega_vita.vpk (SFOS00001) and omega_core_tests.vpk (SFOS00002) in local\build-vita.
#
#   tools\build_vita.ps1 [-Jobs 2] [-Targets omega_vita.vpk,omega_core_tests.vpk] [-Reconfigure] [-StageTests]
#
# -StageTests: copies the repository files omega_core_tests reads (research\, config\, tests\data\) to Vita3K's
# ux0:data/omega_strain/repo (research/VITA.md "Core tests on the Vita"); for the handheld, upload the same folders.
#
# Needs VITASDK (default C:\vitasdk) and the Visual Studio CMake / Ninja (only as build tools; the compiler is
# arm-vita-eabi-gcc). research/VITA.md "Vita3K" shows how to install and run the .vpk files on the emulator.
param(
    [int]$Jobs = 2,
    [string[]]$Targets,
    [switch]$Reconfigure,
    [switch]$StageTests,
    [string]$VitaSdk = $(if ($env:VITASDK) { $env:VITASDK } else { 'C:\vitasdk' }),
    [string]$VisualStudio
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# The play-test repository ships without tests\ (tools\publish): only the app is built there.
$hasTests = Test-Path -LiteralPath (Join-Path $root 'tests\native_tests.cpp')
if (-not $Targets) { $Targets = if ($hasTests) { @('omega_vita.vpk', 'omega_core_tests.vpk') } else { @('omega_vita.vpk') } }
if (-not $VisualStudio) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) { $VisualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
}
$build = Join-Path $root 'local\build-vita'
$cmakeDir = if ($VisualStudio) { Join-Path $VisualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake' } else { $root }
$cmake = Join-Path $cmakeDir 'CMake\bin\cmake.exe'
$ninja = Join-Path $cmakeDir 'Ninja\ninja.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }
if (-not (Test-Path $ninja)) { $ninja = 'ninja' }
$env:VITASDK = $VitaSdk
$env:PATH = "$VitaSdk\bin;$env:PATH"

if ($Reconfigure -or -not (Test-Path (Join-Path $build 'build.ninja'))) {
    & $cmake -S $root -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$($VitaSdk -replace '\\','/')/share/vita.toolchain.cmake" `
        -DCMAKE_BUILD_TYPE=Release "-DBUILD_TESTING=$(if ($hasTests) { 'ON' } else { 'OFF' })"
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
}
& $cmake --build $build --parallel $Jobs --target @Targets
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
foreach ($t in $Targets) { Get-Item (Join-Path $build $t) | Select-Object Name, Length, LastWriteTime }
if ($StageTests) {
    $repo = Join-Path $env:APPDATA 'Vita3K\Vita3K\ux0\data\omega_strain\repo'
    foreach ($dir in 'research', 'config', 'tests\data') {
        $from = Join-Path $root $dir
        if (Test-Path $from) {
            $to = Join-Path $repo $dir
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
            Copy-Item -Recurse -Force $from (Split-Path -Parent $to)
        }
    }
    "staged the test files in $repo"
}
