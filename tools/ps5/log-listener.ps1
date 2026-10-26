param([int]$Port = 18200, [string]$Out = "$PSScriptRoot\..\..\local\ps5-live.log")
# Port-only (132ps5c, research/PS5.md "Paths"): receives the PS5 build's log stream (deploy with -LogHost <this PC's
# address>, and allow TCP $Port through the firewall). Each connection starts a new section; Ctrl+C stops.
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any, $Port)
$listener.Start()
"listening on TCP $Port, writing $Out"
while ($true) {
  $client = $listener.AcceptTcpClient()
  Add-Content $Out ("===== connection " + (Get-Date -Format s) + " from " + $client.Client.RemoteEndPoint)
  $reader = [IO.StreamReader]::new($client.GetStream())
  try { while (($line = $reader.ReadLine()) -ne $null) { Add-Content $Out $line; $line } } catch {}
  $client.Close()
}