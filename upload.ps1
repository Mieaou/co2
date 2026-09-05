<#
.SYNOPSIS
    Compiles and uploads the Multi-Sensor CO2 & Health Monitor firmware to Seeed XIAO nRF52840 Sense.
#>

param(
    [string]$Port = ""
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  Seeed XIAO nRF52840: Multi-Sensor CO2 & Health Monitor Flasher" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""

# 1. Locate bundled arduino-cli
$CliPath = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if (-not (Test-Path $CliPath)) {
    $CliPath = "C:\PROGRA~1\ARDUIN~1\resources\app\lib\backend\resources\arduino-cli.exe"
}
if (-not (Test-Path $CliPath)) {
    Write-Host "[-] ERROR: arduino-cli.exe not found!" -ForegroundColor Red
    Write-Host "    Make sure Arduino IDE 2.x is installed in Program Files." -ForegroundColor Yellow
    exit 1
}

$Fqbn = "Seeeduino:nrf52:xiaonRF52840Sense"
$SketchDir = $PSScriptRoot

# 2. Detect COM port if not provided
if ([string]::IsNullOrWhiteSpace($Port)) {
    Write-Host "[1/3] Scanning for Seeed XIAO nRF52840 board..." -ForegroundColor Yellow
    try {
        $boardList = & $CliPath board list 2>$null
        foreach ($line in $boardList) {
            if ($line -match "xiaonRF52840|XIAO|Sense|Seeed") {
                $Port = ($line -split '\s+')[0]
                break
            }
        }
    } catch {
        # ignore error during detection
    }
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    Write-Host "[!] Auto-detection did not find board. Defaulting to COM5" -ForegroundColor DarkYellow
    $Port = "COM5"
} else {
    Write-Host "[+] Found board on port: $Port" -ForegroundColor Green
}
Write-Host ""

# 3. Compilation
Write-Host "[2/3] Compiling sketch ($Fqbn)..." -ForegroundColor Yellow
$compileOutput = & $CliPath compile --fqbn $Fqbn $SketchDir 2>&1
$compileSuccess = ($LASTEXITCODE -eq 0)

foreach ($line in $compileOutput) {
    Write-Host $line
}

if (-not $compileSuccess) {
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Red
    Write-Host "[-] ERROR: Compilation failed! Check compiler errors above." -ForegroundColor Red
    Write-Host "================================================================" -ForegroundColor Red
    Write-Host ""
    if (-not $env:CI) { Read-Host "Press Enter to exit..." }
    exit 1
}
Write-Host "[+] Compilation successful!" -ForegroundColor Green
Write-Host ""

# 4. Uploading
Write-Host "[3/3] Uploading firmware to port $Port..." -ForegroundColor Yellow
$uploadOutput = & $CliPath upload -p $Port --fqbn $Fqbn $SketchDir 2>&1
$uploadFailed = ($LASTEXITCODE -ne 0)

foreach ($line in $uploadOutput) {
    Write-Host $line
    if ($line -match "Failed to upgrade|could not open port|PermissionError|Error is:|Access is denied") {
        $uploadFailed = $true
    }
}

if ($uploadFailed) {
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Red
    Write-Host "[-] ERROR: Upload to board failed!" -ForegroundColor Red
    Write-Host ""
    Write-Host "Action Required:" -ForegroundColor Yellow
    Write-Host "1. Is the Serial Monitor OPEN in Arduino IDE?" -ForegroundColor White
    Write-Host "   -> Please CLOSE the Serial Monitor in Arduino IDE (it locks $Port)." -ForegroundColor Cyan
    Write-Host "2. Did the USB connection freeze?" -ForegroundColor White
    Write-Host "   -> Unplug and reconnect the USB cable." -ForegroundColor Cyan
    Write-Host "3. Did the COM port change?" -ForegroundColor White
    Write-Host "   -> Run with custom port: .\upload.ps1 -Port COM6" -ForegroundColor Cyan
    Write-Host "================================================================" -ForegroundColor Red
    Write-Host ""
    if (-not $env:CI) { Read-Host "Press Enter to exit..." }
    exit 1
}

Write-Host ""
Write-Host "================================================================" -ForegroundColor Green
Write-Host "  [SUCCESS] Firmware compiled and uploaded successfully!        " -ForegroundColor Green
Write-Host "================================================================" -ForegroundColor Green
Write-Host ""
Write-Host "Serial Monitor Baud Rate: 115200 baud." -ForegroundColor Cyan
Write-Host ""
if (-not $env:CI) { Read-Host "Press Enter to close window..." }
