# ESP32 HID Firmware

A modular Bluetooth Low Energy (BLE) Human Interface Device (HID) firmware built using **ESP-IDF** for the **ESP32-S3**.

This project is a personal exploration of professional embedded firmware architecture, focusing on clean modular design, reusable components, and transport-independent HID implementation. It serves as a foundation for developing Bluetooth input devices such as keyboards, gamepads, custom controllers, and future USB HID devices.

> **Status:** Active Development

---

# Features

- BLE HID implementation using NimBLE
- Keyboard HID over GATT Profile (HOGP)
- Matrix-based input scanning
- Modular HID Core
- Secure BLE pairing and bonding
- HID Report & Boot Protocol support
- Layered firmware architecture
- Component-based ESP-IDF project structure
- Designed for future USB HID support

---

# Firmware Architecture

```
                    Application
                         │
                         ▼
                  Matrix Component
                         │
                         ▼
                  HID Core Component
                         │
                HID Reports & State
                         │
                         ▼
                BLE Transport Component
                         │
                         ▼
                  NimBLE Host Stack
                         │
                         ▼
                      ESP32-S3
```

Each component has a single responsibility.

| Component | Responsibility |
|----------|----------------|
| **main** | Application entry point and firmware coordination |
| **matrix** | GPIO initialization and key matrix scanning |
| **hid_core** | HID report generation, protocol state, report descriptors, LED state, control point |
| **ble_transport** | BLE initialization, GAP, GATT, advertising, pairing, notifications |

---

# Project Structure

```
.
├── components/
│   ├── matrix/
│   │   ├── matrix.c
│   │   ├── matrix.h
│   │   └── CMakeLists.txt
│   │
│   ├── hid_core/
│   │   ├── hid_core.c
│   │   ├── hid_core.h
│   │   └── CMakeLists.txt
│   │
│   └── ble_transport/
│       ├── ble_transport.c
│       ├── ble_transport.h
│       └── CMakeLists.txt
│
├── main/
│   ├── main.c
│   └── CMakeLists.txt
│
├── CMakeLists.txt
├── sdkconfig
├── partitions.csv
└── README.md
```

---

# Development Environment

| Item | Value |
|------|-------|
| MCU | ESP32-S3 |
| Framework | ESP-IDF v5.4.x |
| Bluetooth Stack | NimBLE |
| IDE | Visual Studio Code |
| Language | C |
| Build System | CMake + Ninja |

---

# Build

Build the project

```bash
idf.py build
```

Flash the firmware

```bash
idf.py flash
```

Open serial monitor

```bash
idf.py monitor
```

---

# Current Architecture

The firmware is organized into reusable components following a layered architecture.

```
Application
     │
     ▼
 Matrix Component
     │
     ▼
 HID Core
     │
     ▼
 BLE Transport
     │
     ▼
 NimBLE
```

This separation keeps the HID logic independent of the underlying transport, making future expansion significantly easier.

---

# Design Goals

- Modular firmware architecture
- Transport-independent HID implementation
- Hardware abstraction
- Clean separation of responsibilities
- Reusable embedded software components
- Production-oriented codebase
- Easy addition of future transport layers

---

# Roadmap

Planned enhancements include:

- USB HID (TinyUSB)
- Transport abstraction layer
- Runtime HID profile switching
- Battery Service
- Device Information Service
- OTA Firmware Updates
- Persistent configuration (NVS)
- Configurable key mapping
- Multiple HID report descriptors
- TinyML-based gesture recognition
- Companion desktop/mobile configuration tool

---

# License

This project is intended for learning, experimentation, and embedded firmware development using the ESP-IDF framework.
