# esp32_hid_3x3_gamepad

A modular Bluetooth Low Energy (BLE) Human Interface Device (HID) firmware built using ESP-IDF for the ESP32-S3.

This project is a personal exploration of BLE HID firmware architecture, focusing on scalable design, clean abstractions, and extensibility. It serves as a foundation for building Bluetooth input devices such as gamepads, keyboards, custom controllers, and other HID peripherals.

> **Status:** Active Development

---

## Features

- BLE HID implementation using NimBLE
- Matrix input scanning
- Modular HID core
- Secure pairing and bonding
- Automatic reconnection
- Layered firmware architecture
- ESP-IDF based project

---

## Firmware Architecture

```
          Application
               │
               ▼
         Matrix Scanner
               │
               ▼
          HID Core
               │
               ▼
        BLE Transport
               │
               ▼
         NimBLE Stack
               │
               ▼
           ESP32-S3
```

Each module has a single responsibility:

- **Matrix** – Reads hardware inputs.
- **HID Core** – Generates HID reports.
- **BLE Transport** – Handles Bluetooth communication.
- **Application** – Coordinates the firmware.

---

## Project Structure

```
.
├── main/
│   ├── main.c
│   ├── matrix.c
│   ├── matrix.h
│   ├── hid_core.c
│   ├── hid_core.h
│   ├── ble_transport.c
│   └── ble_transport.h
│
├── CMakeLists.txt
├── sdkconfig
├── partitions.csv
└── README.md
```

---

## Development Environment

| Item | Value |
|------|-------|
| MCU | ESP32-S3 |
| Framework | ESP-IDF |
| Bluetooth Stack | NimBLE |
| IDE | Visual Studio Code |
| Language | C |

---

## Build

```bash
idf.py build
```

Flash

```bash
idf.py flash
```

Monitor

```bash
idf.py monitor
```

---

## Design Goals

- Clean modular architecture
- Reusable firmware components
- Hardware abstraction
- Transport-independent HID core
- Easy feature expansion
- Production-oriented codebase

---

## Future Work

This repository will evolve incrementally as new features are implemented and tested.

Examples include:

- BLE OTA Firmware Updates
- RGB LED Control
- Device Configuration Service
- Persistent Settings (NVS)
- Companion Mobile Application
- Multiple HID Profiles
- Custom HID Report Descriptors
- Battery Service
- Additional HID Device Support

---

## License

This project is intended for learning, experimentation, and embedded firmware development.