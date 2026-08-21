#include "Settings.h"

#include <cstdio>
#include <sys/stat.h>

#include <app/paths.h>
#include <tactility/preferences.h>

namespace {

constexpr const char* PREFERENCES_FILENAME = "settings.properties";

/** Resolve the settings file, creating the app's user data directory if needed. */
bool getPreferencesPath(char* buffer, size_t bufferSize) {
    char directory[192];
    if (app_paths_get_user_data_directory(THERMAL_CAMERA_APP_ID, directory, sizeof(directory)) != ERROR_NONE) {
        return false;
    }
    mkdir(directory, 0755);
    return app_paths_get_user_data_path(THERMAL_CAMERA_APP_ID, PREFERENCES_FILENAME, buffer, bufferSize) ==
           ERROR_NONE;
}

/** preferences_opt_int32 leaves the output untouched when the key is absent. */
int32_t optInt32(const Preferences* preferences, const char* key, int32_t defaultValue) {
    int32_t value = defaultValue;
    preferences_opt_int32(preferences, key, &value);
    return value;
}

bool optBool(const Preferences* preferences, const char* key, bool defaultValue) {
    bool value = defaultValue;
    preferences_opt_bool(preferences, key, &value);
    return value;
}

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
    char path[256];
    if (!getPreferencesPath(path, sizeof(path))) return;

    Preferences* preferences = preferences_open(path);
    if (preferences == nullptr) return;

    settings.palette = static_cast<PaletteId>(optInt32(preferences, "palette", settings.palette));
    settings.interpolation =
        static_cast<InterpolationMode>(optInt32(preferences, "interpolation", settings.interpolation));
    settings.unit = static_cast<TemperatureUnit>(optInt32(preferences, "unit", settings.unit));
    settings.refreshRate = static_cast<MlxRefreshRate>(optInt32(preferences, "rate", settings.refreshRate));
    settings.pattern = static_cast<MlxPattern>(optInt32(preferences, "pattern", settings.pattern));
    settings.resolution = static_cast<MlxResolution>(optInt32(preferences, "adc", settings.resolution));

    settings.emissivity = fromFixed(optInt32(preferences, "emissivity", toFixed(settings.emissivity, 100.0f)), 100.0f);
    settings.reflectedTemperature =
        fromFixed(optInt32(preferences, "reflected", toFixed(settings.reflectedTemperature, 10.0f)), 10.0f);
    settings.temperatureOffset =
        fromFixed(optInt32(preferences, "offset", toFixed(settings.temperatureOffset, 10.0f)), 10.0f);
    settings.noiseFilter = fromFixed(optInt32(preferences, "noise", toFixed(settings.noiseFilter, 100.0f)), 100.0f);

    settings.autoRange = optBool(preferences, "autoRange", settings.autoRange);
    settings.manualRangeMinimum =
        fromFixed(optInt32(preferences, "rangeMin", toFixed(settings.manualRangeMinimum, 10.0f)), 10.0f);
    settings.manualRangeMaximum =
        fromFixed(optInt32(preferences, "rangeMax", toFixed(settings.manualRangeMaximum, 10.0f)), 10.0f);

    settings.mirror = optBool(preferences, "mirror", settings.mirror);
    settings.flip = optBool(preferences, "flip", settings.flip);
    settings.showColorBar = optBool(preferences, "colorBar", settings.showColorBar);
    settings.showCrosshair = optBool(preferences, "crosshair", settings.showCrosshair);

    settings.measureMode = static_cast<MeasureMode>(optInt32(preferences, "measure", settings.measureMode));

    settings.alarmMode = static_cast<AlarmMode>(optInt32(preferences, "alarmMode", settings.alarmMode));
    settings.alarmLow = fromFixed(optInt32(preferences, "alarmLow", toFixed(settings.alarmLow, 10.0f)), 10.0f);
    settings.alarmHigh = fromFixed(optInt32(preferences, "alarmHigh", toFixed(settings.alarmHigh, 10.0f)), 10.0f);

    settings.boxLeft = optInt32(preferences, "boxLeft", settings.boxLeft);
    settings.boxTop = optInt32(preferences, "boxTop", settings.boxTop);
    settings.boxRight = optInt32(preferences, "boxRight", settings.boxRight);
    settings.boxBottom = optInt32(preferences, "boxBottom", settings.boxBottom);
    settings.spotIndex = optInt32(preferences, "spot", settings.spotIndex);

    preferences_close(preferences);
    settingsValidate(settings);
}

void settingsSave(const CameraSettings& settings) {
    char path[256];
    if (!getPreferencesPath(path, sizeof(path))) return;

    Preferences* preferences = preferences_open(path);
    if (preferences == nullptr) return;

    preferences_put_int32(preferences, "palette", settings.palette);
    preferences_put_int32(preferences, "interpolation", settings.interpolation);
    preferences_put_int32(preferences, "unit", settings.unit);
    preferences_put_int32(preferences, "rate", settings.refreshRate);
    preferences_put_int32(preferences, "pattern", settings.pattern);
    preferences_put_int32(preferences, "adc", settings.resolution);

    preferences_put_int32(preferences, "emissivity", toFixed(settings.emissivity, 100.0f));
    preferences_put_int32(preferences, "reflected", toFixed(settings.reflectedTemperature, 10.0f));
    preferences_put_int32(preferences, "offset", toFixed(settings.temperatureOffset, 10.0f));
    preferences_put_int32(preferences, "noise", toFixed(settings.noiseFilter, 100.0f));

    preferences_put_bool(preferences, "autoRange", settings.autoRange);
    preferences_put_int32(preferences, "rangeMin", toFixed(settings.manualRangeMinimum, 10.0f));
    preferences_put_int32(preferences, "rangeMax", toFixed(settings.manualRangeMaximum, 10.0f));

    preferences_put_bool(preferences, "mirror", settings.mirror);
    preferences_put_bool(preferences, "flip", settings.flip);
    preferences_put_bool(preferences, "colorBar", settings.showColorBar);
    preferences_put_bool(preferences, "crosshair", settings.showCrosshair);

    preferences_put_int32(preferences, "measure", settings.measureMode);

    preferences_put_int32(preferences, "alarmMode", settings.alarmMode);
    preferences_put_int32(preferences, "alarmLow", toFixed(settings.alarmLow, 10.0f));
    preferences_put_int32(preferences, "alarmHigh", toFixed(settings.alarmHigh, 10.0f));

    preferences_put_int32(preferences, "boxLeft", settings.boxLeft);
    preferences_put_int32(preferences, "boxTop", settings.boxTop);
    preferences_put_int32(preferences, "boxRight", settings.boxRight);
    preferences_put_int32(preferences, "boxBottom", settings.boxBottom);
    preferences_put_int32(preferences, "spot", settings.spotIndex);

    preferences_close(preferences);
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
