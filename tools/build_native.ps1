param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio discovery tool missing. Install MSVC C++ build tools and CMake support, or use CMake directly.'
}
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'No MSVC x64 build tools installation was found.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$ninja = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
foreach ($tool in @($vcvars, $cmake, $ctest, $ninja)) {
    if (!(Test-Path -LiteralPath $tool)) { throw "Required build tool is missing: $tool" }
}
$build = Join-Path $workspace "local\build-native\$Configuration"
$installer = Split-Path -Parent $vswhere
# The play-test repository ships without tests\ (tools\publish): build only the game and tools there.
$hasTests = Test-Path -LiteralPath (Join-Path $workspace 'tests\native_tests.cpp')
$testing = if ($hasTests) { 'ON' } else { 'OFF' }
$command = "set `"PATH=$installer;%PATH%`" && call `"$vcvars`" >nul && `"$cmake`" -S `"$workspace`" -B `"$build`" -G Ninja `"-DCMAKE_MAKE_PROGRAM=$ninja`" -DCMAKE_BUILD_TYPE=$Configuration -DBUILD_TESTING=$testing && `"$cmake`" --build `"$build`""
if ($hasTests) { $command += " && `"$ctest`" --test-dir `"$build`" --output-on-failure" }
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Native configure/build/tests failed with exit code $LASTEXITCODE" }
Write-Output "The game: $build\OmegaStrain.exe"
Write-Output "Native executable: $build\omega_objectives.exe"
Write-Output "Native asset viewer: $build\omega_assets.exe"
Write-Output "Native geometry viewer: $build\omega_scene.exe"
Write-Output "Native motion probe: $build\omega_motion.exe"
Write-Output "Native face collision probe: $build\omega_collision.exe"
Write-Output "Native character data probe: $build\omega_character.exe"
Write-Output "Native animation channel probe: $build\omega_animation.exe"
Write-Output "Native character animation viewer: $build\omega_actor.exe"
