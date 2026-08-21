# ThermalCamera — Tactility App

A thermal imager application for the **MLX90640** 32×24 IR sensor, built for the [Tactility OS](https://tactilityproject.org) on ESP32.

## Features

| Feature | Detail |
|---|---|
| Sensor | MLX90640 (32 × 24 pixels, −40 °C … +300 °C) |
| Palette | Iron / Rainbow (8-stop gradient) |
| Frame rate | 2 Hz (configurable in `main.c`) |
| Display | Auto-scales each pixel to 6 × 6 px → 192 × 144 px canvas |
| Readouts | Min / Max / Centre-point temperatures |
| Crosshair | White + on the centre pixel |

## Hardware wiring (I2C)

```
MLX90640   →   ESP32
─────────────────────
VCC (3.3 V) →  3V3
GND         →  GND
SDA         →  board default SDA (GPIO21 on most boards)
SCL         →  board default SCL (GPIO22 on most boards)
```

> The sensor I2C address is **0x33** (default, AD0 low).

## Build

### Prerequisites

1. [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/)
2. [Tactility repository](https://github.com/TactilityProject/Tactility) cloned somewhere.

### Steps

```bash
# 1. Clone this app alongside TactilityApps (or standalone)
git clone <this-repo> ThermalCamera
cd ThermalCamera

# 2. Point to Tactility SDK
export TACTILITY_PATH=/path/to/Tactility

# 3. Configure your ESP32 target
idf.py set-target esp32          # or esp32s3, esp32s2, etc.

# 4. Build & flash
idf.py build flash monitor
```

### Registering the app in your board config

In your board's `AppList` (e.g. `Boards/CYD-2432S028/Source/BoardConfig.cpp`), add:

```cpp
#include "thermal_camera/main/Source/main.c"  // or link as idf component

// inside the app list:
&thermal_camera_app,
```

Or use Tactility's App Hub / SD card install mechanism if you package it as a `.app` bundle.

## File structure

```
ThermalCamera/
├── CMakeLists.txt            # Top-level IDF project
├── manifest.properties       # Tactility app metadata
├── README.md
└── main/
    ├── CMakeLists.txt        # IDF component
    └── Source/
        ├── mlx90640.h        # Sensor driver (public API)
        ├── mlx90640.c        # Sensor driver (implementation)
        └── main.c            # LVGL UI + app lifecycle
```

## Calibration note

The driver extracts all calibration constants from the sensor's internal EEPROM on first start (`mlx90640_init`).  
This follows the MLX90640 datasheet (rev 3) calculation steps.  
For production use you may want to store the `Mlx90640Params` struct in NVS to avoid re-reading EEPROM on every boot.

## License

GPL v3 — same as the rest of TactilityApps.
