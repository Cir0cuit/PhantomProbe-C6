# PhantomProbe-C6: Wireless RF Security Research & Telemetry Hardware Probe

[![Platform: ESP-IDF](https://img.shields.io/badge/Platform-ESP--IDF%20v5.1%2B-red.svg)](https://docs.espressif.com/projects/esp-idf/)
[![Hardware: ESP32-C6 RISC-V](https://img.shields.io/badge/Hardware-ESP32--C6%20RISC--V%20160MHz-blue.svg)](https://www.espressif.com/en/products/socs/esp32-c6)
[![Wireless: Wi-Fi 6 & BLE 5](https://img.shields.io/badge/Wireless-802.11ax%20%7C%20BLE%205.0-green.svg)]()
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Purpose: Defensive Cybersecurity](https://img.shields.io/badge/Domain-Defensive%20Security%20Research-purple.svg)]()

**PhantomProbe-C6** is an embedded wireless RF security research probe engineered for the **Espressif ESP32-C6 RISC-V** SoC.

---

### 💡 What does this actually do?

If you flash this firmware onto an ESP32-C6 board and plug it into a USB port, here is what it does:

1. 📡 **Broadcasts multiple virtual Wi-Fi networks simultaneously**:  
   Instead of setting up 10 separate physical routers, a single ESP32-C6 board broadcasts raw Wi-Fi beacon frames into the air to simulate a whole fleet of virtual access points (e.g., `IoT-Sensors-Net`, `Corporate-DMZ`, `Guest-Network`). You configure the names in a simple text file (`ssid_list.conf`).
2. 🔍 **Discovers nearby client devices & audits active probe sweeps**:  
   Monitors 2.4 GHz management traffic, automatically registering newly discovered station MAC addresses in an internal cache (`[NEW STATION #N]`). When phones or laptops perform discovery sweeps (`[WILDCARD SCAN]`), the probe answers with synthetic honeypot responses. When clients broadcast directed probes for saved networks (`[PROBE SNIFF]`), it captures and logs their Preferred Network List (PNL) leaks.
3. 📱 **Bypasses OS Bluetooth discovery filters with realistic gadget profiles**:  
   Modern operating systems (iOS, Android, Windows) ignore generic Bluetooth beacons in standard scan lists. This probe bypasses discovery filters by impersonating real Bluetooth accessories—such as wireless keyboards, mice, gamepads, or smartwatches—rotating between them so they actively appear in nearby Bluetooth menus with authentic device icons and logging connection handshakes (`[BLE CONNECT]`, `[BLE MTU]`, `[BLE GATT READ / WRITE]`).
4. 💻 **Streams real-time security & intrusion telemetry straight to your PC**:  
   Connect the board to your computer with a USB-C cable and open a serial terminal at `115200 baud`. You receive instantaneous alerts for wireless disruption attacks (`[DEAUTH ALERT]` with decoded 802.11 reason codes), unauthorized honeypot connection attempts (`[AUTH INTERCEPT]`), and comprehensive health summaries every 5 seconds (uptime `HH:MM:SS`, unique stations discovered, total frames injected, probe responses, and free heap). The onboard WS2812 RGB LED also flashes coordinated colors for glanceable physical bench diagnostics.

### 🔬 Technical Overview
Under the hood, PhantomProbe-C6 combines low-level **IEEE 802.11 raw frame injection**, **promiscuous management frame analysis** (wildcard sweeps, directed probe replies, PNL sniffing, deauth/disassociation detection, and client station tracking), **multi-identity Bluetooth Low Energy (BLE) peripheral emulation**, and **live high-resolution UART serial telemetry** (supplemented by an onboard status LED). This enables researchers to audit wireless client probing behavior, observe how client devices react to synthetic honeypot networks, detect wireless disruption attacks, and investigate mobile operating system discovery filters.

---

## 🚀 Quickstart & Deployment Guide

> **First time here?**  
> If you are exploring this repository for the first time, **reading the full documentation, technical mechanics, and configuration sections below is strongly advised** before deploying the probe. Understanding how the multi-BSSID engine, BLE accessory emulation, and declarative manifest (`ssid_list.conf`) operate will ensure safe, responsible, and effective research.

---

### Step 1: Clone or Download the Repository

Open your terminal or PowerShell, clone the repository, and enter the project folder:

```bash
git clone https://github.com/Cir0cuit/PhantomProbe-C6.git
cd PhantomProbe-C6
```

*(Alternatively, click the green **Code &rarr; Download ZIP** button at the top of this GitHub page, extract the ZIP archive, and open a terminal inside the extracted directory).*

---

### Step 2: Install Software Prerequisites

1. **Python 3.9+**:
   - **Via Windows Package Manager (`winget`)**:
     ```powershell
     winget install Python.Python.3.12
     ```
   - **Via Official Installer**: Download and install from [python.org](https://www.python.org/downloads/).  
     *(Crucial for Windows: Make sure to check the box: **"Add python.exe to PATH"** before clicking Install!)*
   - **Linux / macOS**: Preinstalled or install via package manager (e.g. `sudo apt install python3 python3-pip`).
2. **PlatformIO Core**:
   - Install PlatformIO via pip in your terminal:
     ```bash
     pip install platformio
     ```
   - Verify the installation:
     ```bash
     pio --version
     ```
3. **USB-to-UART Bridge Driver**:
   - **Official Espressif DevKitC-1 (Silicon Labs CP2102N)**: Download and install the [Silicon Labs CP210x Universal Windows Driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).
   - **Clone / Third-Party Boards (WCH CH343 / CH340)**: Download and install the [WCH CH341SER Windows Driver](http://www.wch-ic.com/downloads/CH341SER_EXE.html).
   - **Linux Users**: Grant your user permission to access serial ports:
     ```bash
     sudo usermod -a -G dialout $USER
     ```
     *(Log out and log back in for permissions to take effect).*

---

### Step 3: Hardware Connection & Port Verification

1. Connect your **ESP32-C6-DevKitC-1** board to your computer using a standard **USB-C data sync cable** (ensure it is not a power-only charging cable).
2. The board features two USB-C ports—**always connect to the port labeled `UART`**:
   - **`UART` Port (Use this)**: Connects to the onboard USB-to-UART bridge for firmware flashing and real-time serial telemetry.
   - **`USB` Port**: Connects directly to native USB-JTAG/CDC (used for low-level hardware debugging).
3. Verify your assigned COM / TTY port:
   - **Windows**: Press <kbd>Win</kbd> + <kbd>X</kbd> &rarr; select **Device Manager** &rarr; expand **Ports (COM & LPT)**. Look for `Silicon Labs CP210x USB to UART Bridge (COMx)` (for example, `COM12` or `COM3`).
   - **Linux**: Run `ls /dev/ttyUSB*` or `dmesg | grep tty` (typically `/dev/ttyUSB0`).
   - **macOS**: Run `ls /dev/cu.usbserial*`.

---

### Step 4: Customize Your Deployment Manifest (Optional)

Before flashing, you can customize your synthetic Wi-Fi networks and Bluetooth peripheral identities by editing `ssid_list.conf` in the project root:

```ini
[wifi]
channel = 1               # 2.4 GHz RF channel (1 to 13)
burst_interval_ms = 50    # Delay between beacon cycles

[networks]
# Virtual honeypot networks to broadcast (one per line, max 32 chars)
IoT-Sensors-Net
IoT-Cameras-Net
Corporate-Guest-DMZ

[bluetooth]
enable_ble = 1
accessory_type = 12       # 12 = Auto-Distribute Showcase (Keyboards, Smartwatches, Mice)
```

*(No C code edits required—the build toolchain automatically compiles `ssid_list.conf` directly into flash during the build process).*

---

### Step 5: Build, Flash & Launch Telemetry Console

#### Option A: One-Click PowerShell Script (Recommended for Windows)
The project includes an automated deployment script that synchronizes your configuration manifest, compiles the firmware, auto-detects your COM port, flashes the board, and connects the serial monitor at 115200 baud:

```powershell
.\flash_firmware.ps1
```

- **PowerShell Execution Policy Tip**: If Windows blocks running scripts, unblock script execution for your current session with:
  ```powershell
  Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope Process
  ```
  Then run `.\flash_firmware.ps1` again.
- **Custom Port Override**: If your board is on a port other than `COM12`:
  ```powershell
  .\flash_firmware.ps1 -ComPort COM5
  ```
  *(If omitted, the script automatically scans your system for connected COM ports!)*

#### Option B: PlatformIO Command-Line Interface (Cross-Platform)
Works on Windows, macOS, and Linux:

```bash
# 1. Compile the firmware
python -m platformio run

# 2. Flash to the ESP32-C6 (replace COM12 with your port, e.g. /dev/ttyUSB0 on Linux)
python -m platformio run -t upload --upload-port COM12

# 3. Launch the serial telemetry console (Exit anytime with Ctrl+])
python -m platformio device monitor --port COM12 --baud 115200
```

#### Option C: Visual Studio Code GUI (PlatformIO Extension)
If you prefer a graphical interface:
1. Install the free **PlatformIO IDE** extension in [Visual Studio Code](https://code.visualstudio.com/).
2. Open the cloned `PhantomProbe-C6` directory in VS Code (**File &rarr; Open Folder...**).
3. Use the PlatformIO status bar icons at the bottom of the window:
   - **Build**: Click the checkmark icon (`✓`)
   - **Upload**: Click the arrow icon (`→`)
   - **Serial Monitor**: Click the plug icon (`🔌`) at 115200 baud.

#### Option D: Standalone Serial Telemetry Monitors
Once flashed, any standard serial terminal configured for **115200 baud, 8-N-1** will display real-time probe telemetry:
- **PuTTY**: Select *Serial*, enter your COM port (e.g. `COM12`), set speed to `115200`.
- **Arduino IDE**: Tools &rarr; Serial Monitor, set the bottom-right dropdown to `115200 baud`.
- **Tera Term** / **Serial Studio**: Connect to `COM12` @ 115200.
- **Linux / macOS Terminal**:
  ```bash
  screen /dev/ttyUSB0 115200
  ```

---

### Step 6: Hardware Troubleshooting & FAQ

- **Board Not Detected by PC / No COM Port Assigned**:
  - Unplug and reconnect the USB-C cable into the port labeled **`UART`**, not `USB`.
  - Try another USB-C cable to confirm data transfer lines exist (many cables are charge-only).
  - Reinstall the Silicon Labs CP210x driver.
- **Upload Timeout / "A fatal error occurred: Failed to connect to ESP32-C6"**:
  - The ESP32-C6 may have booted into an application state that missed the auto-reset signal.
  - **Manual Bootloader Recovery**:
    1. Press and **hold down the `BOOT` button** on the board.
    2. While holding `BOOT`, press and release the **`RST` (Reset)** button.
    3. **Release the `BOOT` button**.
    4. Run `.\flash_firmware.ps1` or `pio run -t upload` again—it will flash immediately!
- **Linux: Permission Denied on `/dev/ttyUSB0`**:
  - Run `sudo usermod -a -G dialout $USER` and re-login to grant serial device permissions.
- **Serial Monitor Displays Scrambled / Garbage Text**:
  - Ensure your terminal baud rate is set to exactly **`115200`**.

---

## 📊 Live Serial Telemetry Output

Upon boot, the hardware probe emits diagnostic telemetry over the serial interface:

```text
============================================================
 PhantomProbe-C6: Wireless RF Security & Telemetry Probe    
 Target Architecture: ESP32-C6 RISC-V 160MHz (Wi-Fi 6 / BLE 5) 
 Mode: Multi-BSSID Honeypot Simulation & BLE Reconnaissance 
============================================================
I (310) probe_config: Parsing PhantomProbe-C6 deployment manifest (3572 bytes)...
I (320) probe_config: Config loaded: Wi-Fi=5 (CH=1), BLE=ON (5 names, Mode=Auto-Distribute (Showcase Multi-Icon)), LED=ON (Brightness=5%)
I (330) probe_led: Initializing WS2812 RGB LED on GPIO 8 (Eye-safe Brightness: 5%)...
I (340) probe_led: RGB LED driver active.
I (350) probe_main: Initializing network interface and default event loop...
I (380) probe_main: Promiscuous 802.11 management frame analyzer & probe responder: ACTIVE
I (390) probe_main: RF PHY transmitter configured on 2.4 GHz Channel 1 with raw 802.11 TX injection.
I (761) probe_beacon: Virtual AP [1]: SSID='IoT-Sensors-Net' -> BSSID=9E:CC:01:44:39:B2 (CH 1)
I (770) probe_beacon: Virtual AP [2]: SSID='IoT-Cameras-Net' -> BSSID=9E:CC:01:44:39:87 (CH 1)
I (778) probe_beacon: Virtual AP [3]: SSID='IoT-Automation-Net' -> BSSID=9E:CC:01:44:39:98 (CH 1)
I (787) probe_beacon: Virtual AP [4]: SSID='IoT-Isolated-DMZ' -> BSSID=9E:CC:01:44:39:ED (CH 1)
I (795) probe_beacon: Virtual AP [5]: SSID='IoT-Guest-Net' -> BSSID=9E:CC:01:44:39:FE (CH 1)
I (803) probe_main: Starting Wi-Fi beacon injection loop (5 virtual BSSIDs, cycle=50 ms, gap=2 ms)...
I (813) probe_ble: Initializing NimBLE for 5 Bluetooth identities (Mode=12)...
I (819) probe_ble:   BLE Identity [1]: 'IoT-Sensors-Net-BLE' -> Keyboard
I (826) probe_ble:   BLE Identity [2]: 'IoT-Cameras-Net-BLE' -> Mouse
I (832) probe_ble:   BLE Identity [3]: 'IoT-Automation-Net-BLE' -> Gamepad
I (838) probe_ble:   BLE Identity [4]: 'IoT-Isolated-DMZ-BLE' -> Joystick
I (845) probe_ble:   BLE Identity [5]: 'IoT-Guest-Net-BLE' -> Remote Control
I (894) probe_ble: Broadcasting BLE [1/5] as Keyboard: 'IoT-Sensors-Net-BLE' (MAC: E2:39:44:01:CF:91)
...
I (914) probe_main: [NEW STATION #1] Discovered client FC:3C:D7:B0:8E:45 | RSSI: -83 dBm
I (914) probe_main: [PROBE SNIFF] Client FC:3C:D7:B0:8E:45 probing foreign SSID 'Corp-Secured' | RSSI: -83 dBm
I (1206) probe_main: [NEW STATION #2] Discovered client 02:C1:A1:C5:A9:1F | RSSI: -85 dBm
I (1206) probe_main: [WILDCARD SCAN] Client 02:C1:A1:C5:A9:1F sweeping RF space | RSSI: -85 dBm | Replying with 5 honeypots
I (2902) probe_ble: Broadcasting BLE [2/5] as Mouse: 'IoT-Cameras-Net-BLE' (MAC: E2:39:44:01:C2:B0)
I (3062) probe_main: [NEW STATION #3] Discovered client 5C:E5:0C:CF:41:95 | RSSI: -67 dBm
I (3062) probe_main: [PROBE SNIFF] Client 5C:E5:0C:CF:41:95 probing foreign SSID 'Home-WiFi' | RSSI: -67 dBm
I (4100) probe_ble: [BLE CONNECT] Client 70:28:8B:12:34:56 paired on handle 0 (itvl=12, latency=0, timeout=500)
I (4120) probe_ble: [BLE MTU] Client on handle 0 negotiated MTU: 256 bytes
I (4150) probe_ble: [BLE GATT READ] Client handle=0 attr_handle=3 (UUID=0x2A29)
W (5210) probe_main: [DEAUTH ALERT] Deauth Frame: Target=FF:FF:FF:FF:FF:FF | Source=9E:CC:01:44:39:B2 | BSSID=9E:CC:01:44:39:B2 (IoT-Sensors-Net) | Reason=3 (STA Leaving) | RSSI: -42 dBm
I (5340) probe_main: [PROBE HIT] Client A4:C3:F0:12:34:56 queried 'IoT-Sensors-Net' -> Injected response | RSSI: -38 dBm
I (5857) probe_main: === RF Probe Telemetry [Uptime: 00:00:05 | Stations: 4 | Frames: 420 | Probe Resp: 5 | Deauths: 1 | Auths: 0 | Free Heap: 252476 B] ===
I (5859) probe_main:   BSSID: 9E:CC:01:44:39:B2 | CH: 1 | Sent: 84     | SSID: IoT-Sensors-Net
I (5868) probe_main:   BSSID: 9E:CC:01:44:39:87 | CH: 1 | Sent: 84     | SSID: IoT-Cameras-Net
I (5876) probe_main:   BSSID: 9E:CC:01:44:39:98 | CH: 1 | Sent: 84     | SSID: IoT-Automation-Net
I (5885) probe_main:   BSSID: 9E:CC:01:44:39:ED | CH: 1 | Sent: 84     | SSID: IoT-Isolated-DMZ
I (5893) probe_main:   BSSID: 9E:CC:01:44:39:FE | CH: 1 | Sent: 84     | SSID: IoT-Guest-Net
```

---

## 🔬 Research & Threat Modeling Objectives

Modern mobile operating systems (Apple iOS, Google Android) and desktop platforms (Microsoft Windows 11) have introduced aggressive background wireless scanning filters. Standard beacon frames, generic advertisements, and naive test networks are frequently ignored, deduplicated, or hidden from user-facing discovery interfaces.

PhantomProbe-C6 was developed as an experimental probe to explore:

1. **Multi-BSSID Honeypot Simulation**: Simulating complex enterprise environments (segmented VLANs, corporate DMZs, IoT networks) using a single 2.4 GHz physical radio via locally administered IEEE 802.11 MAC addresses.
2. **Client Reconnaissance & PNL Auditing**: Passively registering in-range stations (`[NEW STATION #N]`), detecting client discovery sweeps (`[WILDCARD SCAN]`), responding to targeted honeypot queries (`[PROBE HIT]`), and intercepting Preferred Network List (PNL) leaks for foreign/saved networks (`[PROBE SNIFF]`).
3. **OS-Level BLE Discovery Filter Research**: Investigating how consumer operating systems filter Bluetooth advertisements, demonstrating that spoofing Human Interface Device (HID) and standard GATT appearance profiles forces native discovery dialogs to surface virtual peripherals.
4. **Real-Time Intrusion Telemetry & Audit Logging**: Emitting continuous, high-resolution diagnostic telemetry over USB-UART (115200 baud) for deauth/disassociation attack alerts (`[DEAUTH ALERT]`), honeypot connection attempts (`[AUTH INTERCEPT]`), BLE GATT/MTU interaction auditing, and periodic health monitoring (with glanceable physical status provided by the onboard LED).

---

## ⚡ Hardware Architecture & Specifications

The probe is optimized for the **ESP32-C6-DevKitC-1** (N4 / N8) development board:

| Subsystem | Specification |
|---|---|
| **SoC** | Espressif ESP32-C6 (Single-core 32-bit RISC-V @ 160 MHz) |
| **Memory** | 320 KB SRAM, 512 KB ROM, 4 MB / 8 MB Quad-SPI Flash |
| **Wi-Fi Subsystem** | 2.4 GHz Wi-Fi 6 (IEEE 802.11b/g/n/ax), 20 MHz bandwidth |
| **Bluetooth Subsystem**| Bluetooth 5.0 Low Energy (BLE), High-speed 2 Mbps PHY, Coded PHY |
| **Telemetry & Diagnostics** | High-speed USB-UART serial console (115200 baud) + WS2812 status LED (GPIO8) |
| **Configuration** | Declarative text manifest (`ssid_list.conf`) compiled directly into flash |
| **Host Toolchain** | PlatformIO Core / ESP-IDF Framework (FreeRTOS) |

> **Hardware Context — Why ESP32-C6?**  
> The ESP32-C6 incorporates Espressif's latest Wi-Fi 6 MAC and modem, offering granular access to raw frame transmission (`esp_wifi_80211_tx`) and promiscuous packet interception. Unlike older ESP32 chips, the C6 features a high-performance RISC-V core with hardware RMT encoders and native NimBLE stack integration.

---

## 🛠️ Deep Dive: Technical Mechanics

### 1. Wi-Fi Multi-BSSID Raw Frame Injection
- **Synthetic Frame Assembly**: Rather than instantiating heavy software access points, `beacon_generator.c` crafts raw IEEE 802.11 management frames directly in byte buffers.
- **Microsecond Timestamp Tracking**: Employs `esp_timer_get_time()` to inject 64-bit microsecond timestamps into the Fixed Parameters field, ensuring receiving stations do not reject frames as desynchronized.
- **Sequence Control**: Increments a 12-bit sequence counter (`seq_num << 4`) across frames to satisfy station packet ordering checks.
- **Locally Administered BSSID Derivation**: Generates standards-compliant BSSIDs by taking the base hardware MAC, setting bit 1 (Locally Administered) and clearing bit 0 (Unicast). Octets are hashed per virtual network to guarantee uniqueness without colliding with registered hardware vendor OUIs:
  ```c
  memcpy(networks[i].bssid, base_mac, 6);
  networks[i].bssid[0] = (base_mac[0] | 0x02) & 0xFE;
  networks[i].bssid[5] = (uint8_t)(base_mac[5] ^ ((i + 1) * 0x13));
  ```
- **Information Elements (IE)**: Injects Tag 0 (SSID), Tag 1 (Supported Rates: 1, 2, 5.5, 11, 6, 9, 12, 18 Mbps), Tag 3 (DS Parameter / Channel), Tag 5 (TIM), and Tag 50 (Extended Supported Rates: 24, 36, 48, 54 Mbps).

### 2. Promiscuous Management Frame Sniffer & Honeypot Responder
`wifi_promiscuous_rx_cb` listens to raw IEEE 802.11 management traffic (`WIFI_PROMIS_FILTER_MASK_MGMT`) in real time:
- **Station Discovery & Tracking Cache**: An in-memory 64-entry LRU ring buffer tracks unique client MAC addresses detected in the RF environment, outputting real-time discovery events (`[NEW STATION #N]`) with RSSI.
- **Wildcard Probes (`0x40`, Tag Len 0)**: Detects nearby client discovery sweeps (`[WILDCARD SCAN]`). When an interrogating station broadcasts an empty SSID request, the probe responds on behalf of all virtual honeypot networks.
- **Targeted Probes (`0x40`, Tag Len > 0)**: Compares requested SSIDs against the virtual network table. If a match is found, an immediate directed Probe Response is injected (`[PROBE HIT]`).
- **Foreign Probe Sniffing (PNL Leaks)**: Intercepts directed probe requests for external, non-honeypot networks (`[PROBE SNIFF]`), exposing which saved SSIDs client devices are searching for without transmitting.
- **Authentication & Association Interception (`0x00`, `0x20`, `0xB0`)**: Sniffs connection requests targeting any virtual BSSID (`[AUTH INTERCEPT]`), verifying destination MAC matching against active honeypot networks and triggering high-priority LED alert pulses.
- **Deauthentication & Disassociation Interception (`0xC0`, `0xA0`)**: Passively captures teardown and disruption frames (`[DEAUTH ALERT]`), decoding IEEE 802.11 reason codes (e.g. *STA Leaving*, *Previous Auth Invalid*, *4-Way Handshake Timeout*) with transmitter, destination, BSSID, and RSSI.

### 3. Multi-Identity BLE Accessory Emulation
Consumer operating systems ignore generic BLE beacons in standard Bluetooth scan dialogs. To assess OS-level discovery heuristics, `ble_multi_adv.c` implements:
- **Profile Spoofing**: Injects standard GATT Human Interface Device (HID) Service (`0x1812`) and appearance values:
  - Mode 1: Keyboard (`0x03C1`)
  - Mode 2: Mouse (`0x03C2`)
  - Mode 3: Gamepad (`0x03C4`)
  - Mode 4: Joystick (`0x03C3`)
  - Mode 5: Remote Control (`0x03C8`)
  - Mode 6: Smartwatch (`0x00C2`, Current Time `0x1805`)
  - Mode 7: Heart Rate Monitor (`0x0341`, Heart Rate `0x180D`)
  - Mode 8: Thermometer (`0x0300`, Health Thermometer `0x1809`)
  - Mode 9: Cycling Cadence Sensor (`0x0484`, CSC `0x1816`)
  - Mode 10: Digital Stylus (`0x03C6`)
  - Mode 11: Barcode Scanner (`0x03C5`)
  - Mode 12: **Auto-Distribute Showcase Mode** (cycles across identities with distinct icons)
- **Random Static MAC Rotation**: Generates BLE Core Specification compliant random static addresses (two most significant bits set to `0b11`), rotating identities on every dwell cycle:
  ```c
  rnd_addr[0] ^= ((uint8_t)(cur_idx * 0x1F + 0x0D));
  rnd_addr[1] ^= ((uint8_t)(cur_idx * 0x0B + 0x03));
  rnd_addr[5] = (rnd_addr[5] & 0x3F) | 0xC0;
  ```
- **Temporal Dwell Dynamics**: Employs a 2000 ms dwell window per identity. Testing showed mobile OSs miss sub-second rotations due to periodic BLE radio duty cycling.

### 4. Telemetry Architecture: Live UART Console & Status LED

Real-world security research and wireless auditing require high-fidelity telemetry. Nobody stares at an LED while analyzing probe frames or tracking state machines. PhantomProbe-C6 implements a two-tier telemetry architecture:

#### Primary Channel: High-Resolution USB-UART Serial Console (115200 baud)
The primary diagnostic and auditing interface streams continuously over the dedicated USB-UART bridge at **115200 baud, 8-N-1**:
- **Real-Time RF Security & Attack Alerts**:
  - **Station Discovery (`[NEW STATION #N]`)**: Registers new station MAC addresses detected in the 2.4 GHz RF environment with signal strength (RSSI) and tracks them in an internal ring-buffer cache.
  - **Wildcard Scan Telemetry (`[WILDCARD SCAN]`)**: Logs in-range client devices sweeping the 2.4 GHz band for available access points and reports the number of synthetic honeypot responses dispatched.
  - **Foreign Probe Sniffing (`[PROBE SNIFF]`)**: Intercepts directed probe requests for non-honeypot networks, capturing Preferred Network List (PNL) leaks from nearby client devices.
  - **Deauthentication Alerts (`[DEAUTH ALERT]`)**: When an 802.11 deauth/disassoc frame is detected, the probe logs the target MAC, source MAC, BSSID (identifying whether a honeypot network is targeted), decoded reason code (e.g. `STA Leaving`, `Previous Auth Invalid`), and RSSI signal strength.
  - **Client Authentication Intercepts (`[AUTH INTERCEPT]`)**: Logs station MAC addresses attempting connection or association to any virtual honeypot network with RSSI.
  - **Directed Probe Hits (`[PROBE HIT]`)**: Logs when an interrogating client actively queries a specific virtual SSID and triggers a synthetic response.
- **Real-Time BLE Lifecycle Telemetry**: Every identity rotation outputs the newly active GATT profile name, virtual index, and generated Random Static MAC address. Incoming client connection (`[BLE CONNECT]`) and disconnection (`[BLE DISCONNECT]`) events are logged with peer Bluetooth MAC address, connection interval, latency, supervision timeout, and disconnect reason codes. MTU negotiation (`[BLE MTU]`) and GATT attribute operations (`[BLE GATT READ / WRITE]`) are captured in real-time.
- **Periodic RF Telemetry Summaries**: Every 5 seconds, the Wi-Fi injector task emits aggregated metrics logging formatted uptime (`HH:MM:SS`), unique stations discovered, cumulative transmitted beacon frames, satisfied probe responses, intercepted deauth frames, client auth attempts, and FreeRTOS heap memory health (`esp_get_free_heap_size()`), followed by an individual breakdown of packets sent per virtual BSSID.
- **Flood Protection & Throttling**: Alert logging within promiscuous and GATT callbacks is throttled with high-resolution microsecond timers (`esp_timer_get_time()`) to prevent console saturation and task lockups during sustained high-speed deauth floods or probe storms.

#### Auxiliary Channel: WS2812 Hardware Status LED (GPIO8 via RMT)
For standalone bench testing or quick visual verification when a serial monitor is not connected, the onboard WS2812 addressable LED provides glanceable state feedback:
- **Eye-Safe Logarithmic Scaling**: Bare surface-mount WS2812 diodes without diffusers cause intense retina fatigue. The driver maps brightness (1–100%) through an eye-safe curve:
  $$\text{PWM} = 3 + \left\lfloor \frac{\text{brightness} \times 70}{100} \right\rfloor \quad (\text{Hardware Cap: } 75/255)$$
  At default 5% brightness, the duty cycle is ~2.3% (PWM 6/255)—crystal clear indoors with zero eye strain.

#### Hardware Status LED Event Map
| State / Event | Color | RGB Values | Trigger Condition | Priority |
|---|:---:|:---:|---|:---:|
| **Idle (Unconnected)** | `#00FF14` | `(0, 255, 20)` | Radio active, no external client connected | Lowest |
| **Idle (Connected)** | `#00A078` | `(0, 160, 120)` | External client actively paired via BLE | Low |
| **Beacon Burst** | `#00D2FF` | `(0, 210, 255)` | Periodic 802.11 beacon injection cycle | Background |
| **Wildcard Scan** | `#A028FF` | `(160, 40, 255)` | Nearby device performing 802.11 discovery | Medium |
| **Probe Response** | `#FF8C00` | `(255, 140, 0)` | Honeypot replied to client directed probe | Medium-High |
| **BLE Identity Shift** | `#003CFF` | `(0, 60, 255)` | BLE peripheral identity rotation | Medium |
| **Wi-Fi Auth / Assoc** | `#FF0AB4` | `(255, 10, 180)` | External client attempting honeypot auth | High |
| **Wi-Fi Deauth** | `#FF3C00` | `(255, 60, 0)` | 802.11 deauth/disassoc frame intercepted | High |
| **BLE GATT Read** | `#FFFFFF` | `(255, 255, 255)` | Connected client read a GATT characteristic | High |
| **BLE Connect** | `#00FF8C` | `(0, 255, 140)` | BLE client completed connection handshake | Critical |
| **BLE Disconnect** | `#FF1414` | `(255, 20, 20)` | BLE client terminated connection | Critical |

---

## 📋 Declarative Deployment Manifest (`ssid_list.conf`)

PhantomProbe-C6 uses a declarative configuration file at the project root. Edit the file and reflash—the build toolchain automatically synchronizes and compiles it into firmware flash.

```ini
# ============================================================
# PhantomProbe-C6: Wireless RF Security Research & Telemetry Probe
# Target Hardware: ESP32-C6-DevKitC-1 (Wi-Fi 6 / BLE 5 / IEEE 802.15.4)
# ============================================================

[wifi]
# Target 2.4 GHz RF Channel (1 to 13)
channel = 1

# Delay between beacon injection cycles in milliseconds (Default: 50 ms)
burst_interval_ms = 50

# Inter-frame gap between virtual beacons within a burst (Default: 2 ms)
burst_gap_ms = 2

# Active Probe Responder: Reply to client 802.11 Probe Requests (1 = on, 0 = off)
enable_probe_responder = 1

[networks]
# Synthetic targets / honeypot virtual SSIDs (one per line, max 32 chars)
IoT-Sensors-Net
IoT-Cameras-Net
IoT-Automation-Net
IoT-Isolated-DMZ
IoT-Guest-Net

[bluetooth]
# Enable multi-identity BLE advertising (1 = on, 0 = off)
enable_ble = 1

# Rotation dwell time in milliseconds (Default: 2000 ms)
ble_adv_interval_ms = 2000

# Emulated accessory profile:
#   0 = Generic BLE | 1 = Keyboard | 2 = Mouse | 3 = Gamepad | 4 = Joystick
#   5 = Remote | 6 = Smartwatch | 7 = Heart Rate | 8 = Thermometer | 9 = Cycling
#   10 = Stylus | 11 = Barcode Scanner | 12 = Auto-Distribute Showcase
accessory_type = 12

# Custom Bluetooth device names (leave commented to mirror [networks])
# IoT-Sensors-BLE
# IoT-Cameras-BLE

[led]
# Enable WS2812 RGB LED telemetry indicator on GPIO8 (1 = on, 0 = off)
enable_led = 1

# Eye-safe LED brightness percentage (1 to 100, default: 5)
brightness = 5
```

---

## ⚖️ Ethical & Legal Disclaimer

### Defensive Research & Hardware Simulation Scope

> **PhantomProbe-C6 is strictly a passive/synthetic research probe and has ZERO attack or weaponized capacity:**
>
> - **No Deauthentication Capability**: The firmware does **not** generate or inject 802.11 deauth or disassociation frames. Deauth and disassociation frames (`0xC0`, `0xA0`) are strictly intercepted *passively* from the air via promiscuous monitoring to stream real-time intrusion alerts (`[DEAUTH ALERT]`) with decoded 802.11 reason codes over USB-UART and trigger physical LED alert pulses.
> - **No Signal Interference or Jamming**: The hardware lacks any RF jamming or signal overpowering mechanism. It operates strictly as standard low-power 802.11 beacons and BLE peripheral advertisements compliant with typical consumer IoT transmission powers.
> - **No Exploitation Payloads**: The device delivers no exploits, interceptive captive portals, or data theft vectors. It acts solely as an RF environment simulator and discovery heuristic probe for laboratory testing and academic research.

### Responsible Environmental Deployment Guidelines

> When configuring and operating this probe, researchers are expected to adhere to reasonable ethical conduct:
>
> - **No Mischievous Network Flooding**: Do not configure an excessive quantity of synthetic networks in public environments simply to clutter device scan lists, cause nuisance, or create confusion for the public.
> - **Strict Prohibition on Alarmist SSIDs**: **Never** name virtual networks with SSIDs that simulate emergency situations, public alerts, imminent danger, law enforcement operations, or hazards (e.g., false evacuations, bomb threats, or active incident reports).
> - **Limitation of Liability**: The author(s) and contributors assume no liability, responsibility, or legal accountability for misuse, confusion caused by irresponsible SSID naming, or consequences resulting from operating or modifying this codebase.

---

## 📄 License & Acknowledgements

This project is licensed under the **[MIT License](LICENSE)**.

### Upstream Open-Source Projects & Credits
PhantomProbe-C6 builds upon and interfaces with the following open-source technologies:
- **[Espressif ESP-IDF](https://github.com/espressif/esp-idf)**: IoT Development Framework for ESP32-C6 (Licensed under [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0)).
- **[Apache Mynewt NimBLE](https://github.com/apache/mynewt-nimble)**: Open-source Bluetooth 5.0 Low Energy stack integrated into ESP-IDF (Licensed under [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0)).
- **[FreeRTOS](https://www.freertos.org/)**: Real-time operating system kernel for microcontrollers (Licensed under [MIT](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/LICENSE.md)).
- **[PlatformIO](https://platformio.org/)**: Open-source embedded ecosystem and build toolchain (Licensed under [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0)).

*None of the upstream components enforce viral copyleft or license inheritance (no GPL/AGPL dependencies). All linked libraries use permissive open-source licenses (MIT and Apache 2.0), making this repository fully compliant with the MIT License.*
