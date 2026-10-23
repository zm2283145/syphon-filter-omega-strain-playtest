# Port-only (checkpoint 132win8): builds the player-facing release folder of the PC port.
#
#   tools\package_release.ps1 [-Out <folder>] [-Build] [-Zip] [-VsPath <Visual Studio folder>]
#
# The folder (default local\package\OmegaStrain) holds:
#   OmegaStrain.exe              the launcher + front end + play host (local\build-native\Release, target omega_strain)
#   config\disc_us.json          the supported disc's size / SHA-256s (the launcher verifies the user's image with it)
#   local\port_settings.txt      the commented default port settings (tools\package\port_settings.txt)
#   OmegaStrain.map              the linker map (checkpoint 132win12): the crash reporter names the functions of a
#                                crash report's stack with it (crashes\crash-*.txt; no game data)
#   README-PLAY.md               controls, settings keys, fullscreen, CPU partner, cheats, mods, known limitations
#   omega_mod_tool.exe           the mod tool (port-only, checkpoint 132md1, research/MODDING.md): exports textures /
#                                files from the user's disc image into a mod's layout (no game data in the package)
#   MSVCP140*.dll, VCRUNTIME140*.dll   the Visual C++ runtime DLLs OmegaStrain.exe imports (dumpbin /dependents),
#                                copied from the Visual Studio redist folder (app-local deployment)
#   MANIFEST.txt                 every file's size and SHA-256 and the source commit
# No game data is copied: the launcher reads the user's own disc image at run time and derives its tables into
# local\derived. The script fails if anything else ends up in the folder, or if an import of OmegaStrain.exe is
# neither packaged nor a Windows system DLL. Last line "EXIT 0" on success.
param(
    [string]$Out,
    [switch]$Build,
    [switch]$Zip,
    [string]$VsPath
)
$ErrorActionPreference = 'Stop'
if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) { $VsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
    if (-not $VsPath) { $VsPath = 'D:\Program Files\Microsoft Visual Studio\18\Enterprise' }
}
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'local\package\OmegaStrain' }
$Out = [IO.Path]::GetFullPath($Out)
$release = Join-Path $root 'local\build-native\Release'

try {
    if ($Build) {
        $cmake = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        $vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
        & $env:ComSpec /c "call `"$vcvars`" >nul 2>nul && `"$cmake`" --build `"$release`" --parallel 2 --target omega_strain omega_mod_tool"
        if ($LASTEXITCODE -ne 0) { throw "the omega_strain build failed ($LASTEXITCODE)" }
    }
    $exe = Join-Path $release 'OmegaStrain.exe'
    if (-not (Test-Path $exe)) { throw "$exe is missing: build the omega_strain target (or pass -Build)" }

    # dumpbin and the CRT redist of the newest MSVC toolset in the Visual Studio install.
    $dumpbin = Get-ChildItem (Join-Path $VsPath 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Directory.Parent.Parent.Parent.Name } | Select-Object -Last 1
    if (-not $dumpbin) { throw "dumpbin.exe not found under $VsPath\VC\Tools\MSVC (pass -VsPath)" }
    $crt = Get-ChildItem (Join-Path $VsPath 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Parent.Parent.Name -match '^\d+(\.\d+)+$' } |
        Sort-Object { [version]$_.Parent.Parent.Name } | Select-Object -Last 1
    if (-not $crt) { throw "the Visual C++ CRT redist folder was not found under $VsPath\VC\Redist\MSVC" }
    function Get-Dependents([string]$file) {
        $text = & $dumpbin.FullName /nologo /dependents $file
        if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents $file failed" }
        $text | Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1] }
    }

    # A fresh folder (only an earlier package, recognised by its MANIFEST.txt, or an empty folder is replaced).
    if (Test-Path $Out) {
        $existing = @(Get-ChildItem $Out -Force)
        if ($existing.Count -gt 0 -and -not (Test-Path (Join-Path $Out 'MANIFEST.txt'))) {
            throw "$Out exists and is not an earlier package (no MANIFEST.txt); pick a new -Out"
        }
        Remove-Item $Out -Recurse -Force
    }
    New-Item -ItemType Directory -Force (Join-Path $Out 'config'), (Join-Path $Out 'local') | Out-Null

    Copy-Item $exe (Join-Path $Out 'OmegaStrain.exe')
    $map = [IO.Path]::ChangeExtension($exe, '.map')
    if (-not (Test-Path $map)) { throw "$map is missing (the omega_strain target links with /MAP)" }
    Copy-Item $map (Join-Path $Out 'OmegaStrain.map')
    Copy-Item (Join-Path $root 'config\disc_us.json') (Join-Path $Out 'config\disc_us.json')
    Copy-Item (Join-Path $root 'tools\package\port_settings.txt') (Join-Path $Out 'local\port_settings.txt')
    Copy-Item (Join-Path $root 'tools\package\README-PLAY.md') (Join-Path $Out 'README-PLAY.md')
    $modTool = Join-Path $release 'omega_mod_tool.exe'
    if (-not (Test-Path $modTool)) { throw "$modTool is missing: build the omega_mod_tool target (or pass -Build)" }
    Copy-Item $modTool (Join-Path $Out 'omega_mod_tool.exe')

    # The runtime DLLs: every import found in the CRT redist folder, and theirs in turn.
    $packaged = @{}
    $system = @()
    $queue = [System.Collections.Generic.Queue[string]]::new()
    $queue.Enqueue((Join-Path $Out 'OmegaStrain.exe'))
    $queue.Enqueue((Join-Path $Out 'omega_mod_tool.exe'))
    $seen = @{}
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        foreach ($dll in Get-Dependents $file) {
            $key = $dll.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            $redist = Join-Path $crt.FullName $dll
            if (Test-Path $redist) {
                $target = Join-Path $Out (Split-Path -Leaf $redist)
                Copy-Item $redist $target
                $packaged[$key] = $target
                $queue.Enqueue($target)
            } elseif ($key -like 'api-ms-win-*' -or $key -like 'ext-ms-*' -or
                      (Test-Path (Join-Path $env:SystemRoot "System32\$dll"))) {
                $system += $dll
            } else {
                throw "$(Split-Path -Leaf $file) imports $dll, which is neither in the CRT redist ($($crt.FullName)) nor a system DLL"
            }
        }
    }
    Write-Output "Runtime DLLs (app-local, from $($crt.FullName)): $(($packaged.Values | ForEach-Object { Split-Path -Leaf $_ } | Sort-Object) -join ', ')"
    Write-Output "System imports (Windows): $(($system | Sort-Object -Unique) -join ', ')"

    # Only the expected files: no game data (the disc's files or anything derived from them).
    $expected = @('OmegaStrain.exe', 'OmegaStrain.map', 'omega_mod_tool.exe', 'config\disc_us.json', 'local\port_settings.txt', 'README-PLAY.md') +
        @($packaged.Values | ForEach-Object { Split-Path -Leaf $_ })
    $files = @(Get-ChildItem $Out -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($Out.Length + 1) })
    $unexpected = @($files | Where-Object { $expected -notcontains $_ })
    if ($unexpected.Count) { throw "unexpected files in the package: $($unexpected -join ', ')" }
    $gameData = '\.(iso|bin|img|elf|skm|ska|skas|skel|tdx|gui|vag|vpk|pss|ipu|sga|str|manifest|tuning|ps2|mcd)$'
    if ($files | Where-Object { $_ -match $gameData }) { throw 'game data in the package' }

    $commit = (& git -C $root rev-parse HEAD 2>$null)
    $dirty = (& git -C $root status --porcelain --untracked-files=no 2>$null)
    $lines = @("Syphon Filter: The Omega Strain - PC port release folder (port-only packaging, checkpoint 132win8)",
        "source commit: $commit$(if ($dirty) { ' (with uncommitted changes)' })",
        "built: $((Get-Item $exe).LastWriteTimeUtc.ToString('yyyy-MM-dd HH:mm:ss')) UTC",
        "No game data: the launcher reads your own disc image (README-PLAY.md).", '',
        'size sha256 file')
    foreach ($f in ($files | Sort-Object)) {
        $p = Join-Path $Out $f
        $lines += '{0} {1} {2}' -f (Get-Item $p).Length, (Get-FileHash $p -Algorithm SHA256).Hash.ToLowerInvariant(), $f
    }
    [IO.File]::WriteAllLines((Join-Path $Out 'MANIFEST.txt'), $lines, [Text.UTF8Encoding]::new($false))
    $lines | Select-Object -Skip 5 | ForEach-Object { Write-Output "  $_" }
    $total = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum
    Write-Output ("Package: {0} ({1} files, {2:N1} MiB)" -f $Out, ($files.Count + 1), ($total / 1MB))

    if ($Zip) {
        $zipPath = "$Out.zip"
        if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
        Compress-Archive -Path (Join-Path $Out '*') -DestinationPath $zipPath
        Write-Output "Zip: $zipPath ($((Get-Item $zipPath).Length) bytes)"
    }
    Write-Output 'EXIT 0'
} catch {
    Write-Output "package_release failed: $($_.Exception.Message)"
    Write-Output 'EXIT 1'
}
