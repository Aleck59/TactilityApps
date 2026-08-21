# Host tests

These exercise the parts of the app that do not need a display or a sensor: the
MLX90640 calibration maths, the false-colour renderer and the snapshot writer.

A synthetic EEPROM is pushed through the driver's parameter extraction, then a
forward model derived from the datasheet computes the raw ADC word a sensor
would produce for a known object temperature. Feeding that word back through
`calculateTemperatures()` must return the temperature we started from.

Run them on any machine with a C++20 compiler:

```bash
g++ -std=gnu++20 -O1 -Wall -Wextra -I../main/Source -o test_thermal \
    TestThermalCamera.cpp \
    ../main/Source/Mlx90640.cpp \
    ../main/Source/Palette.cpp \
    ../main/Source/ThermalImage.cpp \
    ../main/Source/Snapshot.cpp
./test_thermal
```
