param(
    [string]$ComPort = "COM12",
    [switch]$BuildOnly,
    [switch]$MonitorOnly,
    [int]$BaudRate = 115200
)

$ProjectPath = $PSScriptRoot

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " PhantomProbe-C6: Hardware Probe Flasher & Telemetry Console" -ForegroundColor Cyan
Write-Host " Target: ESP32-C6 RISC-V (Wi-Fi 6 / BLE 5 / IEEE 802.15.4)   " -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# Synchronize declarative configuration manifest safely
python "$ProjectPath\scripts\generate_config.py"

$pioCmd = "python -m platformio"

if (-not $MonitorOnly) {
    Write-Host "`n[1/3] Compiling PhantomProbe-C6 firmware with deployment manifest..." -ForegroundColor Yellow
    Invoke-Expression "$pioCmd run -d `"$ProjectPath`""
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Build failed. Please inspect compiler output." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "[SUCCESS] Firmware compiled successfully!" -ForegroundColor Green
}

if ($BuildOnly) {
    Write-Host "`n[INFO] Build-only mode requested. Exiting." -ForegroundColor Cyan
    exit 0
}

# Verify COM Port exists, otherwise auto-scan
$availablePorts = [System.IO.Ports.SerialPort]::GetPortNames()
if ($availablePorts -notcontains $ComPort) {
    Write-Host "`n[2/3] COM port $ComPort not found. Scanning available ports..." -ForegroundColor Yellow
    if ($availablePorts.Count -eq 1) {
        $ComPort = $availablePorts[0]
        Write-Host "Auto-selected detected port: $ComPort" -ForegroundColor Green
    } elseif ($availablePorts.Count -gt 1) {
        Write-Host "Detected COM ports: $($availablePorts -join ', ')" -ForegroundColor Yellow
        $ComPort = Read-Host "Enter COM port to use (e.g. COM12)"
    } else {
        Write-Host "`n[ERROR] No COM port detected. Please ensure your ESP32-C6 is plugged in." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "`n[2/3] Using COM Port: $ComPort" -ForegroundColor Green
}

if (-not $MonitorOnly) {
    # Release any lingering background serial monitor processes holding an exclusive lock on the COM port
    $staleMonitors = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.CommandLine -match "platformio.*device\s+monitor" -or $_.CommandLine -match "espidf_monitor"
    }
    if ($staleMonitors) {
        Write-Host "`n[INFO] Releasing serial monitor lock on $ComPort (stopping background monitor processes)..." -ForegroundColor Yellow
        $staleMonitors | ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Milliseconds 600
    }

    Write-Host "`n[3/3] Flashing to ESP32-C6 on $ComPort..." -ForegroundColor Yellow
    Invoke-Expression "$pioCmd run -d `"$ProjectPath`" -t upload --upload-port $ComPort"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Flashing failed." -ForegroundColor Red
        Write-Host "Tip: If the port is busy, ensure any external serial terminals (PuTTY, Arduino, etc.) are closed." -ForegroundColor Yellow
        Write-Host "Tip: If connection timed out, hold the BOOT button, click RST, and release BOOT." -ForegroundColor Yellow
        exit $LASTEXITCODE
    }
    Write-Host "[SUCCESS] Flashing complete!" -ForegroundColor Green
}

Write-Host "`nOpening Serial Monitor ($ComPort @ $BaudRate baud)... (Press Ctrl+C to exit)" -ForegroundColor Cyan
Invoke-Expression "$pioCmd device monitor -d `"$ProjectPath`" --port $ComPort --baud $BaudRate"
