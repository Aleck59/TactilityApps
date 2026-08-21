# Thermal Camera

A thermal imaging app for [Tactility](https://tactilityproject.org) built around the
**MLX90640** 32×24 far-infrared sensor array. It aims at what a handheld thermographic
camera does: radiometric measurement with emissivity correction, selectable palettes and
temperature spans, measurement tools, alarm highlighting and snapshot export.

## Features

### Image

- Bilinear upscaling of the 32×24 array to the largest 4:3 image the display fits,
  with an optional 3×3 pre-filter (`Smooth`) or raw sensor pixels (`Off`).
- Eight palettes: Iron, Rainbow, Rainbow HC, White hot, Black hot, Arctic, Lava, Amber.
- Colour bar with live span labels next to the image.
- Horizontal mirror and vertical flip for any sensor mounting orientation.

### Measurement

- **Spot** — a movable crosshair; tap or drag anywhere on the image to place it.
- **Spot + Min/Max** — additionally marks the hottest and coldest pixel of the scene.
- **Box** — a measurement rectangle reported as minimum, average and maximum; tap to
  re-centre it.
- Emissivity (0.10 … 1.00), reflected apparent temperature and a constant offset are
  applied inside the radiometric calculation, not to the displayed number.
- Scene average and the sensor's own die temperature are always shown.
- Celsius, Fahrenheit or Kelvin.

### Temperature span

- **Auto** — follows the scene, smoothed so the colours do not jump between frames, with
  a minimum span so a flat scene stays calm.
- **Manual** — a locked span. Switching away from auto keeps whatever is on screen at
  that moment, which is the usual way to compare two objects.

### Alarms

Pixels above a limit, below a limit or inside a band are painted in a solid alarm colour
on top of the palette, which is how isotherm highlighting works on a real camera.

### Capture

- **Freeze** holds the current image while you read the measurements off it.
- **Save** writes two files into the app's user data directory:
  - `<timestamp>.bmp` — the image exactly as displayed, including the markers.
  - `<timestamp>.csv` — the raw 32×24 temperature array in degrees Celsius, with a
    header carrying emissivity, reflected and ambient temperature and the scene
    statistics, so the measurement can be re-analysed on a computer.

  When the system clock has not been set the files are numbered instead of timestamped.

### Sensor control

Refresh rate (0.5 … 16 Hz), read-out pattern (chess or interleaved) and ADC resolution
(16 … 19 bit) are configurable. Note that a full image needs two sub-pages, so the image
rate is half the sensor rate: 8 Hz gives 4 full frames per second.

A temporal noise filter averages consecutive frames; the strength is adjustable, and the
sensor's factory-marked bad pixels are interpolated from their neighbours.

## Hardware

The app scans every I2C controller the device exposes and uses the first bus that has a
device at address **0x33**, so no configuration is needed. It keeps scanning while
nothing answers, which means the sensor can be plugged in with the app already running.

### Wiring

| MLX90640 | Signal |
|---|---|
| VIN | 3.3 V |
| GND | GND |
| SDA | board I2C data |
| SCL | board I2C clock |

Most breakout boards (Adafruit, Pimoroni, generic GY-MCU90640) already carry the pull-up
resistors the bus needs.

**Elecrow CrowPanel Basic 7.0** exposes I2C on **GPIO19 (SDA)** and **GPIO20 (SCL)** at
400 kHz. That bus is shared with the GT911 touch controller at address 0x5D, which does
not collide with the sensor's 0x33. Check your board's connector silkscreen for where
those two pins are broken out.

A frame is 1664 bytes, so the sensor occupies the bus for a noticeable while. Transfers
are split into 416-byte chunks to leave gaps for the touch controller.

## Measurement accuracy

The MLX90640 is specified at ±1 °C for a black body under laboratory conditions. In
practice the two settings that matter most are:

- **Emissivity.** The default of 0.95 suits most non-metallic surfaces: paint, plastic,
  wood, skin, rubber, paper. Bare or polished metal sits far lower (0.05 … 0.3) and will
  read much colder than it is unless you correct for it or put a piece of matte tape on
  the spot you want to measure.
- **Reflected temperature.** A shiny surface also reflects the temperature of its
  surroundings into the sensor. Set this to the ambient temperature of the room, or to
  the temperature of whatever large hot object is nearby.

The `Offset` setting is a plain constant added to every pixel. Use it to trim the reading
against a reference thermometer, not to compensate for a wrong emissivity.

Let the sensor settle for a few minutes after power-up: its own die warms up and the
readings drift until it reaches equilibrium.

## Building

The app is built with the Tactility build tool from the root of this repository:

```bash
# once, to point at the ESP-IDF installation
. $IDF_PATH/export.sh

python tactility.py Apps/ThermalCamera build esp32s3
```

Installing straight onto a device on the network:

```bash
python tactility.py Apps/ThermalCamera bir <device-ip> esp32s3
```

Or copy `Apps/ThermalCamera/build/ThermalCamera.app` to the device's SD card.

## Tests

`Tests/` contains host-side tests for the sensor maths, the renderer and the snapshot
writer. See `Tests/README.md` for how to run them.

## A note on symbols

An app is loaded as a relocatable ELF, and every call it makes into the firmware is
resolved at load time against a fixed symbol table. A symbol that is not in that table
builds fine — the ELF is linked with `-shared`, so unresolved symbols are allowed — and
then fails at startup with *"Application failed to start: missing symbol"*.

The C++ runtime is only partially exported: `operator new(size_t)` and the sized
`operator delete` are, but the `nothrow` forms and the array forms are not. This app
therefore uses `malloc`/`free` for its scratch buffers and holds the sensor by value.
The device log names the symbol that could not be resolved (`Can't find symbol ...`),
which is the quickest way to diagnose this class of failure.

## File layout

```
Apps/ThermalCamera/
├── CMakeLists.txt
├── manifest.properties
├── main/
│   ├── CMakeLists.txt
│   └── Source/
│       ├── Mlx90640.{h,cpp}      sensor driver and datasheet calibration maths
│       ├── ThermalImage.{h,cpp}  statistics, filtering, false-colour rendering
│       ├── Palette.{h,cpp}       palette generation
│       ├── Settings.{h,cpp}      settings and their persistence
│       ├── Snapshot.{h,cpp}      bitmap and CSV export
│       ├── ThermalCamera.{h,cpp} app lifecycle and the acquisition task
│       ├── CameraView.cpp        live image, readouts and controls
│       ├── SettingsView.cpp      settings screen
│       ├── Ui.h                  layout helpers that scale with the display
│       └── main.cpp              app registration
└── Tests/
```

## Licence

GPL v3, like the rest of TactilityApps.
