/**
 * @file Settings.h
 * @brief User settings of the thermal camera and their persistence.
 */
#pragma once

#include "Mlx90640.h"
#include "Palette.h"
#include "ThermalImage.h"

#include <cstdint>

/** What the on-image measurement tools show. */
enum MeasureMode : uint8_t {
    /** Centre spot only. */
    MEASURE_SPOT = 0,
    /** Centre spot plus the hottest and coldest pixel of the scene. */
    MEASURE_SPOT_MINMAX,
    /** A user placed box, reported as minimum/average/maximum. */
    MEASURE_BOX
};

struct CameraSettings {
    PaletteId palette = PALETTE_IRON;
    InterpolationMode interpolation = INTERP_LINEAR;
    TemperatureUnit unit = UNIT_CELSIUS;
    MlxRefreshRate refreshRate = MLX_RATE_8HZ;
    MlxPattern pattern = MLX_PATTERN_CHESS;
    MlxResolution resolution = MLX_RESOLUTION_18BIT;

    /** Emissivity of the observed surface, 0.10 .. 1.00 */
    float emissivity = 0.95f;
    /** Apparent temperature of the surroundings in degrees Celsius */
    float reflectedTemperature = 22.0f;
    /** Constant correction added to every measured temperature */
    float temperatureOffset = 0.0f;
    /** Strength of the temporal noise filter, 0 (off) .. 1 (maximum) */
    float noiseFilter = 0.5f;

    bool autoRange = true;
    /** Manual span, only used when autoRange is false */
    float manualRangeMinimum = 20.0f;
    float manualRangeMaximum = 40.0f;

    bool mirror = false;
    bool flip = false;
    bool showColorBar = true;
    bool showCrosshair = true;

    MeasureMode measureMode = MEASURE_SPOT_MINMAX;

    AlarmMode alarmMode = ALARM_OFF;
    float alarmLow = 0.0f;
    float alarmHigh = 60.0f;

    /** Measurement box in sensor coordinates, inclusive. */
    int boxLeft = 10;
    int boxTop = 7;
    int boxRight = 21;
    int boxBottom = 16;

    /** Sensor pixel the movable spot marker points at. */
    int spotIndex = (MLX_HEIGHT / 2) * MLX_WIDTH + (MLX_WIDTH / 2);
};

/** Manifest id of this app, used to resolve its storage paths. */
static constexpr const char* THERMAL_CAMERA_APP_ID = "aleck59.thermalcamera";

/** Load settings from persistent storage, keeping defaults for missing keys. */
void settingsLoad(CameraSettings& settings);

/** Store settings in persistent storage. */
void settingsSave(const CameraSettings& settings);

/** Clamp every field to its valid range; call after loading or editing. */
void settingsValidate(CameraSettings& settings);

/** Milliseconds between two sub-page conversions at the configured refresh rate. */
uint32_t settingsSubPageIntervalMs(MlxRefreshRate rate);
