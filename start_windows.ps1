param(
    [string]$RkHost = "192.168.110.86",
    [int]$Instance = -1,
    [int]$HttpPort = 18091,
    [int]$VideoPort = 9001,
    [int]$ControlPort = 9002,
    [switch]$NoBrowser
)

$ErrorActionPreference = "Stop"

if ($Instance -ge 0) {
    $HttpPort = 18091 + ($Instance * 10)
    $VideoPort = 9001 + ($Instance * 10)
    $ControlPort = 9002 + ($Instance * 10)
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$server = Join-Path $root "win_video_server.py"

if (-not (Test-Path -LiteralPath $server)) {
    throw "win_video_server.py was not found: $server"
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command py -ErrorAction SilentlyContinue
}
if (-not $python) {
    throw "Python was not found. Install Python 3 or add it to PATH."
}

Write-Host "Windows video receiver"
if ($Instance -ge 0) {
    Write-Host "Instance:       $Instance"
}
Write-Host "Browser UI:     http://127.0.0.1:$HttpPort/"
Write-Host "Video input:    Windows TCP port $VideoPort"
Write-Host "Control output: RK3568 $RkHost`:$ControlPort"
Write-Host ""
Write-Host "Expected RK3568 bridge behavior:"
Write-Host "  - connect to Windows_IP:$VideoPort and send AVC1/H.264 packets"
Write-Host "  - listen on $ControlPort for JSON control commands"
Write-Host ""

$args = @(
    $server,
    "--http-port", "$HttpPort",
    "--video-port", "$VideoPort",
    "--control-host", $RkHost,
    "--control-port", "$ControlPort"
)

if (-not $NoBrowser) {
    $args += "--open-browser"
}

& $python.Source @args
