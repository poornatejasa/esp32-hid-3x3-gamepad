# GAMR HID Firmware

ESP-IDF firmware for the ESP32-C6 GAMR 3x3 device. It currently provides an
encrypted BLE keyboard HID service and BLE OTA firmware update support. The
code is arranged so BLE and USB remain separate transports while sharing HID
and OTA core logic.

## Current capabilities

- BLE keyboard HID (report and boot protocol)
- Bonded, encrypted BLE links
- BLE OTA update using the normal firmware image
- Standard Device Information Service with a MAC-derived GAMR device ID
- Matrix scanning and HID report generation
- OTA A/B application partitions

The device always boots in normal HID firmware. There is no separate OTA
advertised name and no OTA-only boot mode.

## Project structure

```text
.
|- components/
|  |- matrix/                 # GPIO and key-matrix scanning
|  |- hid_core/               # HID reports, descriptors, and report state
|  |- ota/                    # Transport-independent ESP-IDF OTA session
|  |  |- ota.c
|  |  `- include/ota.h
|  |- ble/
|  |  |- transport/           # NimBLE lifecycle, GAP, advertising, security
|  |  |- services/            # HID, OTA, and Device Information GATT services
|  |  |- control/             # Reserved BLE control commands
|  |  `- include/             # BLE component public headers and UUIDs
|  `- usb/                    # USB transport/service/control skeleton
|     |- transport/
|     |- services/
|     |- control/
|     `- include/
|- main/                      # Application orchestration
|- tools/
|  `- ota_uploader.py         # Development BLE OTA uploader
|- partitions.csv             # OTA slot layout
`- sdkconfig
```

### Ownership boundaries

- `matrix` produces input events.
- `hid_core` builds HID reports and has no BLE or USB dependency.
- `ble/services/ble_hid.c` sends HID reports over BLE.
- `ota/ota.c` owns the OTA partition, write, verification, boot-slot, and
  reboot operations.
- `ble/services/ble_ota.c` only converts encrypted GATT packets into `ota`
  calls. A future `usb/services/usb_ota.c` should use the same `ota` API.

This keeps transport-specific packet handling out of the HID and OTA cores.

## BLE services

| Service | Purpose |
|---|---|
| HID (`0x1812`) | Keyboard reports and boot protocol |
| OTA (`0xFFF0`) | Encrypted OTA control, data, and status |
| Device Information (`0x180A`) | Manufacturer, model, firmware, hardware, and GAMR device ID |
| Control (`0xFFF5`) | Reserved for future device-control commands |

The OTA service uses:

| Characteristic | UUID | Access |
|---|---:|---|
| Control | `0xFFF1` | Encrypted write with response |
| Data | `0xFFF2` | Encrypted write without response |
| Status | `0xFFF3` | Encrypted read / notification |

## Connection policy

The current firmware permits one BLE central connection.

- While disconnected, GAMR advertises as `Poorna_GAMR`.
- Once a central connects, advertising stops.
- The connected central can use HID and the custom OTA/configuration GATT
  services on that same BLE link.
- When it disconnects, GAMR resumes advertising.

For the future Android companion app, this enables HID and OTA from the same
phone connection. A laptop owning the HID connection and a phone performing
OTA are two different centrals; that requires multi-connection support and is
intentionally not enabled in this firmware.

## BLE security baseline

The firmware uses bonded, encrypted BLE. Existing bonds should be deleted from
the host and paired again after changing pairing settings.

The hardware currently has no display, NFC/QR out-of-band channel, or
dedicated pairing-confirmation control. Pairing therefore remains *Just
Works*: traffic is encrypted, but the first pairing is not MITM-authenticated.
Before release, add a deliberate local pairing window and one verified
association method (preferably QR/NFC OOB for this screenless product), then
implement and validate LE Secure Connections with MITM protection and
Secure-Connections-only mode.

## Build and flash

Use an ESP-IDF v5.4.x terminal configured for ESP32-C6:

```powershell
idf.py build
idf.py flash monitor
```

The OTA binary is generated at:

```text
build/hid.bin
```

## BLE OTA update (development)

### Prerequisites

Create and activate the project virtual environment, then install Bleak:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install bleak
```

### Normal update flow

1. Flash the current firmware and reboot the board.
2. Ensure GAMR is not connected as a HID device to another BLE central.
3. Confirm that `Poorna_GAMR` is advertising.
4. Build the new firmware with `idf.py build`.
5. Run:

   ```powershell
   python tools\ota_uploader.py
   ```

6. The uploader connects, pairs if necessary, transfers `build/hid.bin`,
   verifies the image on the ESP, selects the inactive OTA partition, and
   reboots the device into the update.

The uploader uses the negotiated write-without-response payload (up to 512
bytes) and throttles only in short bursts to avoid overflowing the Windows or
NimBLE queues.

On Windows, the uploader also lists paired GAMR devices remembered by the OS.
This lets it attempt GATT access without a fresh advertisement when GAMR is
already connected to the same laptop. Windows decides whether that HID-owned
link is available to the uploader; this does not provide access to a device
connected to a different phone or computer.

### Known-device connection attempt

To attempt a direct Windows GATT connection without scanning:

```powershell
python tools\ota_uploader.py --device 98:A3:16:7E:53:C2
```

This is useful for a known bonded device. It does not override the one-central
limit: Windows may not expose a HID-owned connection to a separate Bleak
process. If this attempt fails, disconnect the HID device and use the normal
scan-based flow. The future Android app should use its saved `BluetoothDevice`
and `connectGatt()` instead.

### Expected uploader result

```text
Connected to OTA firmware.
OTA START sent
Sending firmware...
Progress : 100% (.../...)
Firmware transfer complete in ... seconds.
OTA END sent
OTA REBOOT sent
```

After reboot, the serial log should report the alternate running OTA
partition. Verify HID input again before treating the update as accepted.

## OTA scope and security roadmap

BLE OTA is functionally complete for development: it requires an encrypted
link, writes to the inactive OTA slot, relies on ESP-IDF image validation in
`esp_ota_end`, checks the declared image size and CRC32, then boots that slot.

Secure boot, flash encryption, signed release images, rollback confirmation,
and application-level authorization remain planned production hardening work.
They are intentionally disabled during development so no eFuses are burned.

## Roadmap

- Android companion app for configuration and OTA
- Device Configuration Service (LED, button mode, and persistent settings)
- Battery Service
- USB HID and USB OTA transport adapter
- Multiple HID report profiles (keyboard/gamepad)
- Secure boot, flash encryption, signed OTA, and rollback policy
- TinyML feature modules

## Clone in a new ESP-IDF environment

Use an ESP-IDF v5.4.x terminal on the new machine. The repository does not
ship build output; regenerate it locally so CMake uses that machine's ESP-IDF
path, compiler toolchain, target settings, and Python environment.

```powershell
git clone <company-repository-url>
cd esp32-hid-3x3-gamepad

# Run from an ESP-IDF v5.4.x terminal after its environment is exported.
idf.py fullclean
idf.py reconfigure
idf.py build
```

Then connect the board and flash it:

```powershell
idf.py flash monitor
```

If `idf.py` is not recognized, open Espressif's **ESP-IDF PowerShell** (or run
the `export.ps1` script for the installed ESP-IDF version) before running the
commands above.
