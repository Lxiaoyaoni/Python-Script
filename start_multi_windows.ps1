param(
    [string]$RkHost = "192.168.110.86",
    [int]$Count = 2,
    [string]$LogDir = "",
    [switch]$NoBrowser
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$startScript = Join-Path $root "start_windows.ps1"

if (-not $LogDir) {
    $LogDir = Join-Path $root "logs"
}

if (-not (Test-Path -LiteralPath $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir | Out-Null
}

$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$summaryLog = Join-Path $LogDir "windows_multi_$runId.summary.log"

function Write-Summary {
    param([string]$Message)
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Write-Host $line
    Add-Content -LiteralPath $summaryLog -Value $line
}

if (-not (Test-Path -LiteralPath $startScript)) {
    throw "start_windows.ps1 was not found: $startScript"
}

if ($Count -lt 1) {
    throw "Count must be >= 1"
}

Write-Summary "Starting $Count Windows receiver instance(s)"
Write-Summary "RK3568 control host: $RkHost"
Write-Summary "Log dir: $LogDir"

for ($i = 0; $i -lt $Count; $i++) {
    $httpPort = 18091 + ($i * 10)
    $videoPort = 9001 + ($i * 10)
    $controlPort = 9002 + ($i * 10)

    $stdoutLog = Join-Path $LogDir "windows_instance_${i}_$runId.log"
    $stderrLog = Join-Path $LogDir "windows_instance_${i}_$runId.err.log"
    $pidFile = Join-Path $LogDir "windows_instance_${i}.pid"

    Write-Summary "Instance $i"
    Write-Summary "  Browser UI:  http://127.0.0.1:$httpPort/"
    Write-Summary "  Video input: Windows TCP port $videoPort"
    Write-Summary "  Control out: RK3568 $RkHost`:$controlPort"
    Write-Summary "  stdout: $stdoutLog"
    Write-Summary "  stderr: $stderrLog"

    $args = @(
        "-NoExit",
        "-ExecutionPolicy", "Bypass",
        "-File", $startScript,
        "-RkHost", $RkHost,
        "-Instance", "$i"
    )

    if ($NoBrowser) {
        $args += "-NoBrowser"
    }

    $process = Start-Process powershell.exe `
        -ArgumentList $args `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru

    Set-Content -LiteralPath $pidFile -Value $process.Id
    Write-Summary "  pid: $($process.Id)"
}
