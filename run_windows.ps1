param(
    [string]$RkHost = "192.168.110.86",
    [int]$HttpPort = 18091,
    [int]$VideoPort = 9001,
    [int]$ControlPort = 9002
)

& "$PSScriptRoot\start_windows.ps1" `
    -RkHost $RkHost `
    -HttpPort $HttpPort `
    -VideoPort $VideoPort `
    -ControlPort $ControlPort
