param(
    [string]$LogDir = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $LogDir) {
    $LogDir = Join-Path $root "logs"
}

if (-not (Test-Path -LiteralPath $LogDir)) {
    Write-Host "Log dir not found: $LogDir"
    exit 0
}

$pidFiles = Get-ChildItem -LiteralPath $LogDir -Filter "windows_instance_*.pid" -ErrorAction SilentlyContinue
if (-not $pidFiles) {
    Write-Host "No Windows receiver pid files found in $LogDir"
    exit 0
}

foreach ($file in $pidFiles) {
    $pidText = Get-Content -LiteralPath $file.FullName -Raw
    $pidText = $pidText.Trim()
    if (-not $pidText) {
        Remove-Item -LiteralPath $file.FullName -Force
        continue
    }

    $processId = [int]$pidText
    $proc = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($proc) {
        Write-Host "Stopping PID $processId from $($file.Name)"
        Stop-Process -Id $processId -Force
    } else {
        Write-Host "PID $processId is not running"
    }

    Remove-Item -LiteralPath $file.FullName -Force
}
