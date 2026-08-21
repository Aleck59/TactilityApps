#include "Settings.h"

#include <TactilityCpp/Preferences.h>

namespace {

constexpr const char* PREFERENCES_NAMESPACE = "ThermalCamera";

int clampInt(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

float clampFloat(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

/** Preferences only stores integers, so fixed point values are used for floats. */
int32_t toFixed(float value, float scale) {
    return static_cast<int32_t>(value * scale + (value >= 0.0f ? 0.5f : -0.5f));
}

float fromFixed(int32_t value, float scale) {
    return static_cast<float>(value) / scale;
}

} // namespace

void settingsLoad(CameraSettings& settings) {
    const Preferences preferences(PREFERENCES_NAMESPACE);

    settings.palette = static_cast<PaletteId>(preferences.getInt32("palette", settings.palette));
    settings.interpolation =
        static_cast<InterpolationMode>(preferences.getInt32("interpolation", settings.interpolation));
    settings.unit = static_cast<TemperatureUnit>(preferences.getInt32("unit", settings.unit));
    settings.refreshRate = static_cast<MlxRefreshRate>(preferences.getInt32("rate", settings.refreshRate));
    settings.pattern = static_cast<MlxPattern>(preferences.getInt32("pattern", settings.pattern));
    settings.resolution = static_cast<MlxResolution>(preferences.getInt32("adc", settings.resolution));

    settings.emissivity = fromFixed(preferences.getInt32("emissivity", toFixed(settings.emissivity, 100.0f)), 100.0f);
    settings.reflectedTemperature =
        fromFixed(preferences.getInt32("reflected", toFixed(settings.reflectedTemperature, 10.0f)), 10.0f);
    settings.temperatureOffset =
        fromFixed(preferences.getInt32("offset", toFixed(settings.temperatureOffset, 10.0f)), 10.0f);
    settings.noiseFilter = fromFixed(preferences.getInt32("noise", toFixed(settings.noiseFilter, 100.0f)), 100.0f);

    settings.autoRange = preferences.getBool("autoRange", settings.autoRange);
    settings.manualRangeMinimum =
        fromFixed(preferences.getInt32("rangeMin", toFixed(settings.manualRangeMinimum, 10.0f)), 10.0f);
    settings.manualRangeMaximum =
        fromFixed(preferences.getInt32("rangeMax", toFixed(settings.manualRangeMaximum, 10.0f)), 10.0f);

    settings.mirror = preferences.getBool("mirror", settings.mirror);
    settings.flip = preferences.getBool("flip", settings.flip);
    settings.showColorBar = preferences.getBool("colorBar", settings.showColorBar);
    settings.showCrosshair = preferences.getBool("crosshair", settings.showCrosshair);

    settings.measureMode = static_cast<MeasureMode>(preferences.getInt32("measure", settings.measureMode));

    settings.alarmMode = static_cast<AlarmMode>(preferences.getInt32("alarmMode", settings.alarmMode));
    settings.alarmLow = fromFixed(preferences.getInt32("alarmLow", toFixed(settings.alarmLow, 10.0f)), 10.0f);
    settings.alarmHigh = fromFixed(preferences.getInt32("alarmHigh", toFixed(settings.alarmHigh, 10.0f)), 10.0f);

    settings.boxLeft = preferences.getInt32("boxLeft", settings.boxLeft);
    settings.boxTop = preferences.getInt32("boxTop", settings.boxTop);
    settings.boxRight = preferences.getInt32("boxRight", settings.boxRight);
    settings.boxBottom = preferences.getInt32("boxBottom", settings.boxBottom);
    settings.spotIndex = preferences.getInt32("spot", settings.spotIndex);

    settingsValidate(settings);
}

void settingsSave(const CameraSettings& settings) {
    const Preferences preferences(PREFERENCES_NAMESPACE);

    preferences.putInt32("palette", settings.palette);
    preferences.putInt32("interpolation", settings.interpolation);
    preferences.putInt32("unit", settings.unit);
    preferences.putInt32("rate", settings.refreshRate);
    preferences.putInt32("pattern", settings.pattern);
    preferences.putInt32("adc", settings.resolution);

    preferences.putInt32("emissivity", toFixed(settings.emissivity, 100.0f));
    preferences.putInt32("reflected", toFixed(settings.reflectedTemperature, 10.0f));
    preferences.putInt32("offset", toFixed(settings.temperatureOffset, 10.0f));
    preferences.putInt32("noise", toFixed(settings.noiseFilter, 100.0f));

    preferences.putBool("autoRange", settings.autoRange);
    preferences.putInt32("rangeMin", toFixed(settings.manualRangeMinimum, 10.0f));
    preferences.putInt32("rangeMax", toFixed(settings.manualRangeMaximum, 10.0f));

    preferences.putBool("mirror", settings.mirror);
    preferences.putBool("flip", settings.flip);
    preferences.putBool("colorBar", settings.showColorBar);
    preferences.putBool("crosshair", settings.showCrosshair);

    preferences.putInt32("measure", settings.measureMode);

    preferences.putInt32("alarmMode", settings.alarmMode);
    preferences.putInt32("alarmLow", toFixed(settings.alarmLow, 10.0f));
    preferences.putInt32("alarmHigh", toFixed(settings.alarmHigh, 10.0f));

    preferences.putInt32("boxLeft", settings.boxLeft);
    preferences.putInt32("boxTop", settings.boxTop);
    preferences.putInt32("boxRight", settings.boxRight);
    preferences.putInt32("boxBottom", settings.boxBottom);
    preferences.putInt32("spot", settings.spotIndex);
}

void settingsValidate(CameraSettings& settings) {
    if (settings.palette >= PALETTE_COUNT) settings.palette = PALETTE_IRON;
    if (settings.interpolation > INTERP_SMOOTH) settings.interpolation = INTERP_LINEAR;
    if (settings.unit > UNIT_KELVIN) settings.unit = UNIT_CELSIUS;
    if (settings.refreshRate > MLX_RATE_16HZ) settings.refreshRate = MLX_RATE_8HZ;
    if (settings.pattern > MLX_PATTERN_CHESS) settings.pattern = MLX_PATTERN_CHESS;
    if (settings.resolution > MLX_RESOLUTION_19BIT) settings.resolution = MLX_RESOLUTION_18BIT;
    if (settings.measureMode > MEASURE_BOX) settings.measureMode = MEASURE_SPOT_MINMAX;
    if (settings.alarmMode > ALARM_BETWEEN) settings.alarmMode = ALARM_OFF;

    settings.emissivity = clampFloat(settings.emissivity, 0.10f, 1.00f);
    settings.reflectedTemperature = clampFloat(settings.reflectedTemperature, -40.0f, 300.0f);
    settings.temperatureOffset = clampFloat(settings.temperatureOffset, -20.0f, 20.0f);
    settings.noiseFilter = clampFloat(settings.noiseFilter, 0.0f, 1.0f);

    settings.manualRangeMinimum = clampFloat(settings.manualRangeMinimum, -40.0f, 300.0f);
    settings.manualRangeMaximum = clampFloat(settings.manualRangeMaximum, -40.0f, 300.0f);
    if (settings.manualRangeMaximum < settings.manualRangeMinimum + 1.0f) {
        settings.manualRangeMaximum = settings.manualRangeMinimum + 1.0f;
    }

    settings.alarmLow = clampFloat(settings.alarmLow, -40.0f, 300.0f);
    settings.alarmHigh = clampFloat(settings.alarmHigh, -40.0f, 300.0f);
    if (settings.alarmHigh < settings.alarmLow) {
        const float swap = settings.alarmLow;
        settings.alarmLow = settings.alarmHigh;
        settings.alarmHigh = swap;
    }

    settings.boxLeft = clampInt(settings.boxLeft, 0, MLX_WIDTH - 1);
    settings.boxRight = clampInt(settings.boxRight, 0, MLX_WIDTH - 1);
    settings.boxTop = clampInt(settings.boxTop, 0, MLX_HEIGHT - 1);
    settings.boxBottom = clampInt(settings.boxBottom, 0, MLX_HEIGHT - 1);
    if (settings.boxRight < settings.boxLeft) {
        const int swap = settings.boxLeft;
        settings.boxLeft = settings.boxRight;
        settings.boxRight = swap;
    }
    if (settings.boxBottom < settings.boxTop) {
        const int swap = settings.boxTop;
        settings.boxTop = settings.boxBottom;
        settings.boxBottom = swap;
    }

    settings.spotIndex = clampInt(settings.spotIndex, 0, MLX_PIXELS - 1);
}

uint32_t settingsSubPageIntervalMs(MlxRefreshRate rate) {
    switch (rate) {
        case MLX_RATE_0_5HZ:
            return 2000;
        case MLX_RATE_1HZ:
            return 1000;
        case MLX_RATE_2HZ:
            return 500;
        case MLX_RATE_4HZ:
            return 250;
        case MLX_RATE_8HZ:
            return 125;
        case MLX_RATE_16HZ:
            return 63;
        case MLX_RATE_32HZ:
            return 32;
        case MLX_RATE_64HZ:
        default:
            return 16;
    }
}
