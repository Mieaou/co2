<#
.SYNOPSIS
    Connects to the Seeed XIAO nRF52840 serial monitor at 115200 baud.
#>

param(
    [string]$Port = ""
)

$CliPath = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if (-not (Test-Path $CliPath)) {
    $CliPath = "C:\PROGRA~1\ARDUIN~1\resources\app\lib\backend\resources\arduino-cli.exe"
}

if (-not (Test-Path $CliPath)) {
    Write-Host "[-] ERROR: arduino-cli.exe not found!" -ForegroundColor Red
    exit 1
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    try {
        $boardList = & $CliPath board list 2>$null
        foreach ($line in $boardList) {
            if ($line -match "xiaonRF52840") {
                $Port = ($line -split '\s+')[0]
                break
            }
        }
    } catch {
        # ignore error
    }
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = "COM5"
}

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  Serial Monitor: Seeed XIAO nRF52840 on $Port (115200 baud)   " -ForegroundColor Cyan
Write-Host "  Press Ctrl+C to disconnect and close.                         " -ForegroundColor Yellow
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.ReadTimeout = 4000

try {
    $sp.Open()
    Write-Host "[+] Connected to $Port! Port is open and listening." -ForegroundColor Green
    Write-Host "[*] Continuous real-time stream active (updates every 3s)..." -ForegroundColor Yellow
    Write-Host "----------------------------------------------------------------" -ForegroundColor DarkGray

    $silentSeconds = 0
    while ($true) {
        try {
            $line = $sp.ReadLine()
            if (-not [string]::IsNullOrWhiteSpace($line)) {
                $silentSeconds = 0
                Write-Host $line
            }
        } catch [System.TimeoutException] {
            $silentSeconds += 4
            if ($silentSeconds -ge 8) {
                Write-Host "[!] No data received in $($silentSeconds)s. If nothing appears:" -ForegroundColor DarkYellow
                Write-Host "    1. Run upload.bat to flash the latest firmware." -ForegroundColor Gray
                Write-Host "    2. Make sure Arduino IDE Serial Monitor is closed (it locks the port)." -ForegroundColor Gray
            }
        }
    }
} catch {
    Write-Host "[-] Connection error: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.Exception.Message -match "Access is denied") {
        Write-Host "    Port $Port is locked! Close any other Serial Monitors or Arduino IDE." -ForegroundColor Yellow
    }
} finally {
    if ($sp) {
        if ($sp.IsOpen) {
            $sp.Close()
        }
        $sp.Dispose()
        Write-Host ""
        Write-Host "[*] Disconnected from $Port and serial handle released." -ForegroundColor Cyan
    }
}
