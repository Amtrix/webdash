# PowerShell TCP Port Forwarder for WSL2
# Forwards traffic from Windows LAN IP to WSL localhost
param(
    [int]$ListenPort = 8089,
    [string]$TargetHost = "localhost",
    [int]$TargetPort = 8089
)

$ErrorActionPreference = "Stop"

# Get LAN IP to bind to (not 0.0.0.0 since WSL uses that)
$lanIP = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like '192.168.*' -or $_.IPAddress -like '10.*' } | Select-Object -First 1).IPAddress
if (-not $lanIP) {
    Write-Host "Could not detect LAN IP" -ForegroundColor Red
    exit 1
}

Write-Host "Port Forwarder: ${lanIP}:$ListenPort -> ${TargetHost}:$TargetPort" -ForegroundColor Cyan

$endpoint = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($lanIP), $ListenPort)
$listener = New-Object System.Net.Sockets.TcpListener $endpoint

# Allow address reuse to bind even if 0.0.0.0 is in use
$listener.Server.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket, [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)

try {
    $listener.Start()
} catch {
    Write-Host "Failed to bind to port $ListenPort - $_" -ForegroundColor Red
    exit 1
}

Write-Host "Listening..." -ForegroundColor Green

function Forward-TcpStream($source, $dest) {
    $buffer = New-Object byte[] 8192
    try {
        while (($bytesRead = $source.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $dest.Write($buffer, 0, $bytesRead)
            $dest.Flush()
        }
    } catch {}
}

while ($true) {
    try {
        $client = $listener.AcceptTcpClient()
        Write-Host "Connection from $($client.Client.RemoteEndPoint)" -ForegroundColor Gray

        $target = New-Object System.Net.Sockets.TcpClient
        $target.Connect($TargetHost, $TargetPort)

        $clientStream = $client.GetStream()
        $targetStream = $target.GetStream()

        # Forward client->target in background runspace
        $runspace1 = [runspacefactory]::CreateRunspace()
        $runspace1.Open()
        $ps1 = [powershell]::Create()
        $ps1.Runspace = $runspace1
        $ps1.AddScript({
            param($src, $dst)
            $buf = New-Object byte[] 8192
            try {
                while (($n = $src.Read($buf, 0, $buf.Length)) -gt 0) {
                    $dst.Write($buf, 0, $n)
                }
            } catch {} finally { $src.Close(); $dst.Close() }
        }).AddArgument($clientStream).AddArgument($targetStream) | Out-Null
        $ps1.BeginInvoke() | Out-Null

        # Forward target->client in background runspace
        $runspace2 = [runspacefactory]::CreateRunspace()
        $runspace2.Open()
        $ps2 = [powershell]::Create()
        $ps2.Runspace = $runspace2
        $ps2.AddScript({
            param($src, $dst)
            $buf = New-Object byte[] 8192
            try {
                while (($n = $src.Read($buf, 0, $buf.Length)) -gt 0) {
                    $dst.Write($buf, 0, $n)
                }
            } catch {} finally { $src.Close(); $dst.Close() }
        }).AddArgument($targetStream).AddArgument($clientStream) | Out-Null
        $ps2.BeginInvoke() | Out-Null

    } catch {
        Write-Host "Error: $_" -ForegroundColor Red
    }
}
