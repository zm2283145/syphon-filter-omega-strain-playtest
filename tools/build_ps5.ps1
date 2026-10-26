param([string]$Target = 'omega_ps5_package', [int]$Jobs = 4, [switch]$Clean)
# Port-only (132ps5c, research/PS5.md "Build"): builds the PS5 target in WSL (Ubuntu-24.04 with the toolchain of
# tools/ps5/setup_ps5_toolchain.sh). The sources are mirrored into ~/ps5work/src (the CMake globs and the compiles are
# slow over /mnt), configured with cmake/ps5/ps5.toolchain.cmake in ~/ps5work/omega, and the folder title is left in
# ~/ps5work/omega/app/dist/PPSA99720 (copied to local\build-ps5\PPSA99720).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$wsl = '/mnt/' + $repo.Substring(0, 1).ToLower() + $repo.Substring(2).Replace('\', '/')
$script = @"
set -e
src='$wsl'
mirror=`$HOME/ps5work/src
out=`$HOME/ps5work/omega
$(if ($Clean) { 'rm -rf $out' })
mkdir -p `$mirror `$out
rsync -a --delete --exclude __pycache__ "`$src/src" "`$src/cmake" "`$src/tools" "`$src/config" "`$src/CMakeLists.txt" `$mirror/
find `$mirror/tools -name '*.sh' -exec sed -i 's/\r`$//' {} +
chmod +x `$mirror/tools/ps5/*.sh
[ -f `$out/build.ninja ] || cmake -S `$mirror -B `$out -G Ninja -DCMAKE_TOOLCHAIN_FILE=`$mirror/cmake/ps5/ps5.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build `$out --target $Target -- -j $Jobs
"@
$script | wsl -d Ubuntu-24.04 -- bash -s
if ($LASTEXITCODE) { throw "the PS5 build failed ($LASTEXITCODE)" }
if ($Target -eq 'omega_ps5_package') {
  $dest = Join-Path $repo 'local\build-ps5'
  New-Item -ItemType Directory -Force $dest | Out-Null
  wsl -d Ubuntu-24.04 -- bash -c "rm -rf '$wsl/local/build-ps5/PPSA99720' && cp -r ~/ps5work/omega/app/dist/PPSA99720 '$wsl/local/build-ps5/'"
  "PS5 folder title: $dest\PPSA99720"
}