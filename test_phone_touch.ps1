param(
    [string]$RkHost = "192.168.110.86",
    [int]$Instance = -1,
    [int]$ControlPort = 9002,
    [ValidateSet("tap", "down", "move", "up", "swipe", "doubletap")]
    [string]$Type = "tap",
    [int]$X = 360,
    [int]$Y = 784,
    [int]$Width = 720,
    [int]$Height = 1568,
    [int]$X2 = 360,
    [int]$Y2 = 500,
    [int]$Duration = 800,
    [int]$Steps = 24
)

$ErrorActionPreference = "Stop"

if ($Instance -ge 0) {
    $ControlPort = 9002 + ($Instance * 10)
}

function Send-ControlJson {
    param(
        [string]$HostName,
        [int]$Port,
        [string]$Json
    )

    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.Connect($HostName, $Port)
        $stream = $client.GetStream()
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Json + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
    } finally {
        $client.Close()
    }
}

function Send-Payload {
    param([hashtable]$Payload)

    $json = $Payload | ConvertTo-Json -Compress
    Write-Host "Payload: $json"
    Send-ControlJson -HostName $RkHost -Port $ControlPort -Json $json
}

Write-Host "Send to RK3568: $RkHost`:$ControlPort"

if ($Type -eq "swipe") {
    if ($Steps -lt 1) {
        $Steps = 1
    }

    $sleepMs = [Math]::Max(1, [int]($Duration / ($Steps + 1)))

    Send-Payload -Payload ([ordered]@{
        type = "down"
        x = $X
        y = $Y
        width = $Width
        height = $Height
    })
    Start-Sleep -Milliseconds $sleepMs

    for ($i = 1; $i -le $Steps; $i++) {
        $nx = [int]($X + (($X2 - $X) * $i / $Steps))
        $ny = [int]($Y + (($Y2 - $Y) * $i / $Steps))
        Send-Payload -Payload ([ordered]@{
            type = "move"
            x = $nx
            y = $ny
            width = $Width
            height = $Height
        })
        Start-Sleep -Milliseconds $sleepMs
    }

    Send-Payload -Payload ([ordered]@{
        type = "up"
        x = $X2
        y = $Y2
        width = $Width
        height = $Height
    })
} else {
    Send-Payload -Payload ([ordered]@{
        type = $Type
        x = $X
        y = $Y
        width = $Width
        height = $Height
    })
}

Write-Host "Done. Check RK3568 terminal for: control: ... result=0"
