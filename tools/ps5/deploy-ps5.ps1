param(
  [Parameter(Mandatory)][string]$App,          # the folder title: <build>/app/dist/PPSA99720 (or out\132ps5c\app\PPSA99720)
  [string]$Ps5 = '10.1.1.93',
  [int]$Port = 1337,
  [string]$Iso,                                # optional: the user's disc image, uploaded as omega_strain.iso (/app0)
  [string]$LogHost,                            # optional: this PC's IPv4 address for the TCP 18200 log stream
  [switch]$EbootOnly
)
# Port-only (132ps5c, research/PS5.md "Deploy"): uploads the PS5 folder title to /data/homebrew/<TITLE_ID> over the
# console's FTP server. eboot.bin and param.json go last, so the title is never seen half-copied. -Iso uploads the
# disc image first (large; skipped when the same size is already there); -LogHost writes log_host.txt into the title
# (read at start: the log is also streamed to tools/ps5/log-listener.ps1). -EbootOnly replaces only eboot.bin (close
# the game first). The coordinator runs this; the agents never connect to the console.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $App).Path
$tid = Split-Path $root -Leaf
if ($tid -notmatch '^PPSA\d{5}$') { throw "the folder must be the title folder (PPSA99720), got $tid" }
$base = "ftp://${Ps5}:$Port/data/homebrew/$tid"
function Upload([string]$file, [string]$rel) {
  curl.exe -s --max-time 3600 --ftp-create-dirs -T $file "$base/$rel"
  if ($LASTEXITCODE) { throw "upload failed: $rel (curl $LASTEXITCODE; 25 usually means the game is running)" }
}
if ($Iso -and -not $EbootOnly) {
  $isoFile = (Resolve-Path $Iso).Path
  $size = (Get-Item $isoFile).Length
  $remote = curl.exe -s --max-time 30 -I "$base/omega_strain.iso" 2>$null | Select-String 'Content-Length: (\d+)'
  if ($remote -and [int64]$remote.Matches[0].Groups[1].Value -eq $size) { "disc image already there ($size bytes)" }
  else { "uploading the disc image ($([math]::Round($size / 1GB, 2)) GB)..."; Upload $isoFile 'omega_strain.iso' }
}
if ($LogHost) {
  $tmp = Join-Path $env:TEMP 'log_host.txt'
  [IO.File]::WriteAllText($tmp, $LogHost)
  Upload $tmp 'log_host.txt'
}
$files = Get-ChildItem $root -Recurse -File |
  Where-Object { -not $EbootOnly -or $_.Name -eq 'eboot.bin' } |
  Sort-Object { if ($_.Name -in 'eboot.bin', 'param.json') { 1 } else { 0 } }
foreach ($f in $files) {
  $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
  Upload $f.FullName $rel
}
"deployed $tid ($($files.Count) files) to $base"