/**
 * @file ThermalImage.h
 * @brief Analysis and false-colour rendering of a 32x24 temperature frame.
 *
 * Everything here works on plain buffers so it can be exercised without a
 * display or a sensor attached.
 */
#pragma once

#include "Mlx90640.h"
#include "Palette.h"

#include <cstdint>

enum TemperatureUnit : uint8_t {
    UNIT_CELSIUS = 0,
    UNIT_FAHRENHEIT,
    UNIT_KELVIN
};

enum InterpolationMode : uint8_t {
    /** One sensor pixel becomes one solid block. */
    INTERP_NONE = 0,
    /** Bilinear upscaling. */
    INTERP_LINEAR,
    /** 3x3 pre-filter followed by bilinear upscaling. */
    INTERP_SMOOTH
};

/** Highlighting of temperatures of interest, drawn on top of the palette. */
enum AlarmMode : uint8_t {
    ALARM_OFF = 0,
    ALARM_ABOVE,
    ALARM_BELOW,
    ALARM_BETWEEN
};

/** Statistics of one temperature frame. */
struct FrameStats {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float average = 0.0f;
    /** Standard deviation, a useful indicator of scene contrast. */
    float deviation = 0.0f;
    int minimumIndex = 0;
    int maximumIndex = 0;
};

/** Statistics of a rectangular region, in sensor pixel coordinates. */
struct RegionStats {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float average = 0.0f;
    int minimumIndex = 0;
    int maximumIndex = 0;
    int pixelCount = 0;
};

/** Everything the renderer needs to turn temperatures into pixels. */
struct RenderOptions {
    /** PALETTE_LEVELS entries in the display's native RGB565 layout. */
    const uint16_t* palette = nullptr;
    float rangeMinimum = 20.0f;
    float rangeMaximum = 40.0f;
    InterpolationMode interpolation = INTERP_LINEAR;
    /** Mirror horizontally; useful when the sensor faces the user. */
    bool mirror = false;
    /** Flip vertically; the MLX90640 is often mounted upside down. */
    bool flip = false;
    AlarmMode alarmMode = ALARM_OFF;
    float alarmLow = 0.0f;
    float alarmHigh = 100.0f;
    uint16_t alarmColor = 0xF81F;
};

/** Convert a temperature in degrees Celsius to the selected unit. */
float thermalConvertUnit(float celsius, TemperatureUnit unit);

/** Convert a value in the selected unit back to degrees Celsius. */
float thermalConvertToCelsius(float value, TemperatureUnit unit);

/** Short unit suffix, e.g. "C". */
const char* thermalUnitSuffix(TemperatureUnit unit);

/** Compute minimum, maximum, average and deviation over a whole frame. */
void thermalComputeStats(const float* frame, FrameStats& stats);

/** Compute statistics over an inclusive rectangle in sensor coordinates. */
void thermalComputeRegionStats(const float* frame, int x0, int y0, int x1, int y1, RegionStats& stats);

/**
 * @brief Sample the frame at fractional sensor coordinates using bilinear weights.
 * Coordinates outside the array are clamped to the edge.
 */
float thermalSample(const float* frame, float x, float y);

/**
 * @brief Blend a newly converted frame into an accumulator to suppress noise.
 * @param accumulator the running average, updated in place
 * @param frame the freshly converted frame
 * @param weight contribution of the new frame (0 < weight <= 1); 1 disables filtering
 */
void thermalTemporalFilter(float* accumulator, const float* frame, float weight);

/** Apply a normalised 3x3 low-pass kernel; @p output may not alias @p frame. */
void thermalSpatialFilter(const float* frame, float* output);

/**
 * @brief Pre-resolved horizontal source coordinates for one output width.
 *
 * They are identical for every row, so the renderer takes them as a table that
 * the caller rebuilds only when the width or the mirror setting changes.
 */
struct ColumnMap {
    int16_t low;
    int16_t high;
    float weight;
};

/** Fill @p map with @p outputWidth entries for the given width and mirroring. */
void thermalBuildColumnMap(int outputWidth, bool mirror, ColumnMap* map);

/**
 * @brief Render a temperature frame into an RGB565 buffer.
 *
 * The output is a straight scaled mapping of the sensor array, so
 * @p outputWidth and @p outputHeight should keep the 4:3 aspect ratio.
 *
 * @param[in] frame MLX_PIXELS temperatures in degrees Celsius
 * @param[out] output outputWidth * outputHeight RGB565 pixels, rows are contiguous
 * @param[in] columnMap table of outputWidth entries from thermalBuildColumnMap()
 */
void thermalRender(
    const float* frame,
    uint16_t* output,
    int outputWidth,
    int outputHeight,
    const RenderOptions& options,
    const ColumnMap* columnMap
);

/** Fill @p output with a top-to-bottom gradient of the palette (hot at the top). */
void thermalRenderColorBar(const uint16_t* palette, uint16_t* output, int width, int height);

/** Map a sensor pixel index to its centre in output pixel coordinates. */
void thermalSensorToOutput(
    int sensorIndex,
    int outputWidth,
    int outputHeight,
    bool mirror,
    bool flip,
    int& outX,
    int& outY
);

/** Map output pixel coordinates back to the nearest sensor pixel. */
int thermalOutputToSensor(
    int x,
    int y,
    int outputWidth,
    int outputHeight,
    bool mirror,
    bool flip
);

/** Draw a crosshair with a one pixel dark outline so it stays visible on any palette. */
void thermalDrawCrosshair(uint16_t* buffer, int width, int height, int x, int y, int arm, uint16_t color);

/** Draw an unfilled rectangle, clipped to the buffer. */
void thermalDrawRect(uint16_t* buffer, int width, int height, int x0, int y0, int x1, int y1, uint16_t color);

/** Draw a small square marker centred on the given point. */
void thermalDrawMarker(uint16_t* buffer, int width, int height, int x, int y, int size, uint16_t color);
