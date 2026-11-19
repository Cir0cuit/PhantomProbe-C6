param(
    [ValidatePattern('^(COM\d+)?$')]
    [string]$ComPort = "",
    [switch]$BuildOnly,
    [switch]$MonitorOnly,
    [int]$BaudRate = 115200
)

$ProjectPath = $PSScriptRoot

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " PhantomProbe-C6: Hardware Probe Flasher & Telemetry Console" -ForegroundColor Cyan
Write-Host " Target: ESP32-C6 RISC-V (Wi-Fi 6 / BLE 5 / IEEE 802.15.4)   " -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# Returns the command that runs PlatformIO Core as an array (executable, then leading arguments).
function Find-PlatformIO {
    foreach ($name in 'pio', 'platformio') {
        $cmd = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { return , @($cmd.Source) }
    }
    $penv = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\platformio.exe'
    if (Test-Path $penv) { return , @($penv) }

    # PlatformIO installed with pip: look for the module in every Python on this machine
    $pythons = @(Get-Command python, python3 -CommandType Application -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source })
    if (Get-Command py -CommandType Application -ErrorAction SilentlyContinue) {
        $pythons += @(& py -0p 2>$null | ForEach-Object { if ($_ -match '([A-Za-z]:\\.*python[\w.]*\.exe)\s*$') { $Matches[1] } })
    }
    foreach ($python in ($pythons | Select-Object -Unique)) {
        & $python -c "import platformio" 2>$null
        if ($LASTEXITCODE -eq 0) { return , @($python, '-m', 'platformio') }
    }
    return $null
}

# Lists serial ports that belong to an ESP32 board: USB-UART bridges first, native USB Serial/JTAG last.
function Get-BoardPorts {
    $known = [ordered]@{
        'VID_10C4' = 'UART port (Silicon Labs CP210x bridge)'
        'VID_1A86' = 'UART port (WCH CH34x bridge)'
        'VID_303A' = 'USB port (native USB Serial/JTAG)'
    }
    $devices = Get-CimInstance Win32_PnPEntity -Filter "PNPClass='Ports'" -ErrorAction SilentlyContinue
    foreach ($vid in $known.Keys) {
        foreach ($device in $devices) {
            if ($device.PNPDeviceID -match "^USB\\$vid" -and $device.Name -match '\((COM\d+)\)') {
                [pscustomobject]@{ Port = $Matches[1]; Kind = $known[$vid] }
            }
        }
    }
}

$pio = Find-PlatformIO
if (-not $pio) {
    Write-Host "`n[ERROR] PlatformIO Core not found. Install it with: pip install platformio" -ForegroundColor Red
    exit 1
}
$pioExe = $pio[0]
$pioArgs = @($pio | Select-Object -Skip 1)
Write-Host "Using PlatformIO: $($pio -join ' ')" -ForegroundColor DarkGray

if (-not $MonitorOnly) {
    Write-Host "`n[1/3] Compiling PhantomProbe-C6 firmware with deployment manifest..." -ForegroundColor Yellow
    & $pioExe @pioArgs run -d $ProjectPath
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Build failed. Please inspect the output above." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "[SUCCESS] Firmware compiled successfully!" -ForegroundColor Green
}

if ($BuildOnly) {
    Write-Host "`n[INFO] Build-only mode requested. Exiting." -ForegroundColor Cyan
    exit 0
}

# Pick the serial port
if ($ComPort) {
    if ([System.IO.Ports.SerialPort]::GetPortNames() -notcontains $ComPort) {
        Write-Host "`n[ERROR] $ComPort does not exist. Check Device Manager > Ports (COM & LPT)." -ForegroundColor Red
        exit 1
    }
    Write-Host "`n[2/3] Using COM Port: $ComPort" -ForegroundColor Green
} else {
    $boards = @(Get-BoardPorts)
    if ($boards.Count -eq 0) {
        Write-Host "`n[ERROR] No ESP32 board detected. Plug the board in with a data cable, or pass -ComPort COMx." -ForegroundColor Red
        exit 1
    }
    if ($boards.Count -gt 1) {
        # Could be one board on both of its ports, or a second device: let the user decide
        Write-Host "`n[2/3] Several candidate ports detected:" -ForegroundColor Yellow
        $boards | ForEach-Object { Write-Host "  $($_.Port)  $($_.Kind)" }
        if ([Console]::IsInputRedirected) {
            Write-Host "[ERROR] Pass the port to use with -ComPort COMx." -ForegroundColor Red
            exit 1
        }
        $answer = (Read-Host "Port to use [$($boards[0].Port)]").Trim().ToUpper()
        if (-not $answer) { $answer = $boards[0].Port }
        if (@($boards | ForEach-Object { $_.Port }) -notcontains $answer) {
            Write-Host "[ERROR] $answer is not one of the detected ports." -ForegroundColor Red
            exit 1
        }
        $ComPort = $answer
        Write-Host "[2/3] Using COM Port: $ComPort" -ForegroundColor Green
    } else {
        $ComPort = $boards[0].Port
        Write-Host "`n[2/3] Auto-selected $ComPort - $($boards[0].Kind)" -ForegroundColor Green
    }
}

if (-not $MonitorOnly) {
    # A PlatformIO serial monitor left open on this port would block the upload
    $staleMonitors = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessId -ne $PID -and
        $_.CommandLine -match 'device\s+monitor' -and
        $_.CommandLine -match 'platformio|\bpio(\.exe)?\b' -and
        ($_.CommandLine -match "$ComPort(?!\d)" -or $_.CommandLine -notmatch '(?<!\S)(--port[\s=]|-p\s*\S)')
    }
    foreach ($monitor in $staleMonitors) {
        Write-Host "[INFO] Stopping serial monitor (PID $($monitor.ProcessId)) that may hold ${ComPort}: $($monitor.CommandLine)" -ForegroundColor Yellow
        Stop-Process -Id $monitor.ProcessId -Force -ErrorAction SilentlyContinue
    }
    if ($staleMonitors) { Start-Sleep -Milliseconds 600 }

    Write-Host "`n[3/3] Flashing to ESP32-C6 on $ComPort..." -ForegroundColor Yellow
    & $pioExe @pioArgs run -d $ProjectPath -t upload --upload-port $ComPort
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n[ERROR] Flashing failed." -ForegroundColor Red
        Write-Host "Tip: If the port is busy, ensure any external serial terminals (PuTTY, Arduino, etc.) are closed." -ForegroundColor Yellow
        Write-Host "Tip: If connection timed out, hold the BOOT button, click RST, and release BOOT." -ForegroundColor Yellow
        exit $LASTEXITCODE
    }
    Write-Host "[SUCCESS] Flashing complete!" -ForegroundColor Green
}

Write-Host "`nOpening Serial Monitor ($ComPort @ $BaudRate baud)... (Press Ctrl+C to exit)" -ForegroundColor Cyan
& $pioExe @pioArgs device monitor -d $ProjectPath --port $ComPort --baud $BaudRate
