param([string]$Target = 'omega_ps5_package', [int]$Jobs = 4, [switch]$Clean, [switch]$Imports,
      [string]$Work = '~/ps5work', [string]$Toolchain = '~/ps5-app-toolchain')
# Port-only (132ps5c, 132ps5d; research/PS5.md "Build"): builds the PS5 target in WSL (Ubuntu-24.04 with clang-18,
# lld-18, llvm-18, cmake, ninja, python3, rsync, git, make, unzip). tools/ps5/fetch_toolchain.sh fetches the pinned
# toolchain into -Toolchain first (nothing to do when it is current). The sources are mirrored into <Work>/src (the
# CMake globs and the compiles are slow over /mnt), configured with cmake/ps5/ps5.toolchain.cmake in <Work>/omega, and
# the folder title <Work>/omega/PPSA99720 is copied to local\build-ps5\PPSA99720 (with ps5\llvm-pie.elf for symbols).
# -Imports also regenerates src/port/ps5/ps5_imports.cpp from the linked title (the omega_ps5_imports target) and
# copies it back into this checkout; build again afterwards until it no longer changes.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$wsl = '/mnt/' + $repo.Substring(0, 1).ToLower() + $repo.Substring(2).Replace('\', '/')
$work = $Work.Replace('~', '$HOME')
$toolchain = $Toolchain.Replace('~', '$HOME')
$script = @"
set -e
export PATH=/usr/local/bin:/usr/bin:/bin
src='$wsl'
mirror=$work/src
out=$work/omega
export PS5_TOOLCHAIN=$toolchain
bash "`$src/tools/ps5/fetch_toolchain.sh" "`$PS5_TOOLCHAIN"
$(if ($Clean) { 'rm -rf "$out"' })
mkdir -p "`$mirror" "`$out"
rsync -a --delete --exclude __pycache__ "`$src/src" "`$src/cmake" "`$src/tools" "`$src/config" "`$src/CMakeLists.txt" "`$mirror/"
find "`$mirror/tools" -name '*.sh' -exec sed -i 's/\r`$//' {} +
[ -f "`$out/build.ninja" ] || cmake -S "`$mirror" -B "`$out" -G Ninja -DCMAKE_TOOLCHAIN_FILE="`$mirror/cmake/ps5/ps5.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release -DPS5_TOOLCHAIN="`$PS5_TOOLCHAIN"
cmake --build "`$out" --target $Target -- -j $Jobs
$(if ($Imports) { 'cmake --build "$out" --target omega_ps5_imports && cp "$mirror/src/port/ps5/ps5_imports.cpp" "$mirror/src/port/ps5/ps5_imports.hpp" "$src/src/port/ps5/"' })
if [ '$Target' = omega_ps5_package ]; then
  rm -rf "`$src/local/build-ps5/PPSA99720" "`$src/local/build-ps5/ps5"
  mkdir -p "`$src/local/build-ps5/ps5"
  cp -r "`$out/PPSA99720" "`$src/local/build-ps5/"
  cp "`$out/ps5/llvm-pie.elf" "`$src/local/build-ps5/ps5/"
fi
"@
# Run from a file: through stdin, the build's own commands would read the rest of the script.
New-Item -ItemType Directory -Force (Join-Path $repo 'local') | Out-Null
[IO.File]::WriteAllText((Join-Path $repo 'local\ps5_build.sh'), ($script -replace "`r`n", "`n"))
wsl -d Ubuntu-24.04 -- bash "$wsl/local/ps5_build.sh"
if ($LASTEXITCODE) { throw "the PS5 build failed ($LASTEXITCODE)" }
if ($Target -eq 'omega_ps5_package') { "PS5 folder title: $(Join-Path $repo 'local\build-ps5\PPSA99720')" }
