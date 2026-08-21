#include "ThermalImage.h"

#include <cmath>

namespace {

inline int clampInt(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

inline float clampFloat(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

inline bool alarmMatches(const RenderOptions& options, float temperature) {
    switch (options.alarmMode) {
        case ALARM_ABOVE:
            return temperature >= options.alarmHigh;
        case ALARM_BELOW:
            return temperature <= options.alarmLow;
        case ALARM_BETWEEN:
            return temperature >= options.alarmLow && temperature <= options.alarmHigh;
        case ALARM_OFF:
        default:
            return false;
    }
}

inline void putPixel(uint16_t* buffer, int width, int height, int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    buffer[y * width + x] = color;
}

} // namespace

float thermalConvertUnit(float celsius, TemperatureUnit unit) {
    switch (unit) {
        case UNIT_FAHRENHEIT:
            return celsius * 1.8f + 32.0f;
        case UNIT_KELVIN:
            return celsius + 273.15f;
        case UNIT_CELSIUS:
        default:
            return celsius;
    }
}

float thermalConvertToCelsius(float value, TemperatureUnit unit) {
    switch (unit) {
        case UNIT_FAHRENHEIT:
            return (value - 32.0f) / 1.8f;
        case UNIT_KELVIN:
            return value - 273.15f;
        case UNIT_CELSIUS:
        default:
            return value;
    }
}

const char* thermalUnitSuffix(TemperatureUnit unit) {
    switch (unit) {
        case UNIT_FAHRENHEIT:
            return "F";
        case UNIT_KELVIN:
            return "K";
        case UNIT_CELSIUS:
        default:
            return "C";
    }
}

void thermalComputeStats(const float* frame, FrameStats& stats) {
    float minimum = frame[0];
    float maximum = frame[0];
    int minimumIndex = 0;
    int maximumIndex = 0;
    float sum = 0.0f;
    float sumOfSquares = 0.0f;

    for (int i = 0; i < MLX_PIXELS; i++) {
        const float value = frame[i];
        if (value < minimum) {
            minimum = value;
            minimumIndex = i;
        }
        if (value > maximum) {
            maximum = value;
            maximumIndex = i;
        }
        sum += value;
        sumOfSquares += value * value;
    }

    const float count = static_cast<float>(MLX_PIXELS);
    const float average = sum / count;
    float variance = sumOfSquares / count - average * average;
    if (variance < 0.0f) variance = 0.0f;

    stats.minimum = minimum;
    stats.maximum = maximum;
    stats.average = average;
    stats.deviation = sqrtf(variance);
    stats.minimumIndex = minimumIndex;
    stats.maximumIndex = maximumIndex;
}

void thermalComputeRegionStats(const float* frame, int x0, int y0, int x1, int y1, RegionStats& stats) {
    if (x0 > x1) {
        const int swap = x0;
        x0 = x1;
        x1 = swap;
    }
    if (y0 > y1) {
        const int swap = y0;
        y0 = y1;
        y1 = swap;
    }
    x0 = clampInt(x0, 0, MLX_WIDTH - 1);
    x1 = clampInt(x1, 0, MLX_WIDTH - 1);
    y0 = clampInt(y0, 0, MLX_HEIGHT - 1);
    y1 = clampInt(y1, 0, MLX_HEIGHT - 1);

    float minimum = frame[y0 * MLX_WIDTH + x0];
    float maximum = minimum;
    int minimumIndex = y0 * MLX_WIDTH + x0;
    int maximumIndex = minimumIndex;
    float sum = 0.0f;
    int count = 0;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const int index = y * MLX_WIDTH + x;
            const float value = frame[index];
            if (value < minimum) {
                minimum = value;
                minimumIndex = index;
            }
            if (value > maximum) {
                maximum = value;
                maximumIndex = index;
            }
            sum += value;
            count++;
        }
    }

    stats.minimum = minimum;
    stats.maximum = maximum;
    stats.average = count > 0 ? sum / static_cast<float>(count) : 0.0f;
    stats.minimumIndex = minimumIndex;
    stats.maximumIndex = maximumIndex;
    stats.pixelCount = count;
}

float thermalSample(const float* frame, float x, float y) {
    x = clampFloat(x, 0.0f, static_cast<float>(MLX_WIDTH - 1));
    y = clampFloat(y, 0.0f, static_cast<float>(MLX_HEIGHT - 1));

    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = clampInt(x0 + 1, 0, MLX_WIDTH - 1);
    const int y1 = clampInt(y0 + 1, 0, MLX_HEIGHT - 1);
    const float weightX = x - static_cast<float>(x0);
    const float weightY = y - static_cast<float>(y0);

    const float top = frame[y0 * MLX_WIDTH + x0] * (1.0f - weightX) + frame[y0 * MLX_WIDTH + x1] * weightX;
    const float bottom = frame[y1 * MLX_WIDTH + x0] * (1.0f - weightX) + frame[y1 * MLX_WIDTH + x1] * weightX;
    return top * (1.0f - weightY) + bottom * weightY;
}

void thermalTemporalFilter(float* accumulator, const float* frame, float weight) {
    weight = clampFloat(weight, 0.02f, 1.0f);
    if (weight >= 1.0f) {
        for (int i = 0; i < MLX_PIXELS; i++) accumulator[i] = frame[i];
        return;
    }
    const float keep = 1.0f - weight;
    for (int i = 0; i < MLX_PIXELS; i++) {
        accumulator[i] = accumulator[i] * keep + frame[i] * weight;
    }
}

void thermalSpatialFilter(const float* frame, float* output) {
    // Slightly weighted towards the centre so real hot spots keep their peak.
    static const float kernel[3][3] = {
        {0.0625f, 0.125f, 0.0625f},
        {0.1250f, 0.250f, 0.1250f},
        {0.0625f, 0.125f, 0.0625f}
    };

    for (int y = 0; y < MLX_HEIGHT; y++) {
        for (int x = 0; x < MLX_WIDTH; x++) {
            float sum = 0.0f;
            float weightSum = 0.0f;
            for (int dy = -1; dy <= 1; dy++) {
                const int sy = y + dy;
                if (sy < 0 || sy >= MLX_HEIGHT) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    const int sx = x + dx;
                    if (sx < 0 || sx >= MLX_WIDTH) continue;
                    const float weight = kernel[dy + 1][dx + 1];
                    sum += frame[sy * MLX_WIDTH + sx] * weight;
                    weightSum += weight;
                }
            }
            output[y * MLX_WIDTH + x] = weightSum > 0.0f ? sum / weightSum : frame[y * MLX_WIDTH + x];
        }
    }
}

void thermalBuildColumnMap(int outputWidth, bool mirror, ColumnMap* map) {
    if (map == nullptr || outputWidth <= 0) return;
    const float scaleX = static_cast<float>(MLX_WIDTH) / static_cast<float>(outputWidth);

    for (int x = 0; x < outputWidth; x++) {
        const int sourceX = mirror ? (outputWidth - 1 - x) : x;
        float position = (static_cast<float>(sourceX) + 0.5f) * scaleX - 0.5f;
        position = clampFloat(position, 0.0f, static_cast<float>(MLX_WIDTH - 1));
        const int low = static_cast<int>(position);
        map[x].low = static_cast<int16_t>(low);
        map[x].high = static_cast<int16_t>(clampInt(low + 1, 0, MLX_WIDTH - 1));
        map[x].weight = position - static_cast<float>(low);
    }
}

void thermalRender(
    const float* frame,
    uint16_t* output,
    int outputWidth,
    int outputHeight,
    const RenderOptions& options,
    const ColumnMap* columnMap
) {
    if (frame == nullptr || output == nullptr || options.palette == nullptr || columnMap == nullptr) return;
    if (outputWidth <= 0 || outputHeight <= 0) return;

    float span = options.rangeMaximum - options.rangeMinimum;
    if (span < 0.1f) span = 0.1f;
    const float levelScale = static_cast<float>(PALETTE_LEVELS - 1) / span;
    const bool interpolate = options.interpolation != INTERP_NONE;
    const bool alarmActive = options.alarmMode != ALARM_OFF;
    const float scaleY = static_cast<float>(MLX_HEIGHT) / static_cast<float>(outputHeight);

    for (int y = 0; y < outputHeight; y++) {
        const int sourceY = options.flip ? (outputHeight - 1 - y) : y;
        float positionY = (static_cast<float>(sourceY) + 0.5f) * scaleY - 0.5f;
        positionY = clampFloat(positionY, 0.0f, static_cast<float>(MLX_HEIGHT - 1));
        const int rowLow = static_cast<int>(positionY);
        const int rowHigh = clampInt(rowLow + 1, 0, MLX_HEIGHT - 1);
        const float weightY = interpolate ? positionY - static_cast<float>(rowLow) : 0.0f;

        const float* topRow = frame + rowLow * MLX_WIDTH;
        const float* bottomRow = frame + rowHigh * MLX_WIDTH;
        uint16_t* outputRow = output + static_cast<size_t>(y) * static_cast<size_t>(outputWidth);

        for (int x = 0; x < outputWidth; x++) {
            const ColumnMap& column = columnMap[x];

            float temperature;
            if (interpolate) {
                const float top = topRow[column.low] +
                                  (topRow[column.high] - topRow[column.low]) * column.weight;
                const float bottom = bottomRow[column.low] +
                                     (bottomRow[column.high] - bottomRow[column.low]) * column.weight;
                temperature = top + (bottom - top) * weightY;
            } else {
                temperature = topRow[column.low];
            }

            if (alarmActive && alarmMatches(options, temperature)) {
                outputRow[x] = options.alarmColor;
                continue;
            }

            int level = static_cast<int>((temperature - options.rangeMinimum) * levelScale);
            level = clampInt(level, 0, PALETTE_LEVELS - 1);
            outputRow[x] = options.palette[level];
        }
    }
}

void thermalRenderColorBar(const uint16_t* palette, uint16_t* output, int width, int height) {
    if (palette == nullptr || output == nullptr || width <= 0 || height <= 0) return;

    for (int y = 0; y < height; y++) {
        // Top of the bar is the hot end, matching the labels next to it.
        int level = static_cast<int>(
            static_cast<float>(height - 1 - y) * static_cast<float>(PALETTE_LEVELS - 1) /
            static_cast<float>(height > 1 ? height - 1 : 1)
        );
        level = clampInt(level, 0, PALETTE_LEVELS - 1);
        const uint16_t color = palette[level];
        uint16_t* row = output + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; x++) row[x] = color;
    }
}

void thermalSensorToOutput(
    int sensorIndex,
    int outputWidth,
    int outputHeight,
    bool mirror,
    bool flip,
    int& outX,
    int& outY
) {
    const int sensorX = sensorIndex % MLX_WIDTH;
    const int sensorY = sensorIndex / MLX_WIDTH;

    const float stepX = static_cast<float>(outputWidth) / static_cast<float>(MLX_WIDTH);
    const float stepY = static_cast<float>(outputHeight) / static_cast<float>(MLX_HEIGHT);

    int x = static_cast<int>((static_cast<float>(sensorX) + 0.5f) * stepX);
    int y = static_cast<int>((static_cast<float>(sensorY) + 0.5f) * stepY);
    if (mirror) x = outputWidth - 1 - x;
    if (flip) y = outputHeight - 1 - y;

    outX = clampInt(x, 0, outputWidth - 1);
    outY = clampInt(y, 0, outputHeight - 1);
}

int thermalOutputToSensor(int x, int y, int outputWidth, int outputHeight, bool mirror, bool flip) {
    if (outputWidth <= 0 || outputHeight <= 0) return 0;
    if (mirror) x = outputWidth - 1 - x;
    if (flip) y = outputHeight - 1 - y;

    int sensorX = static_cast<int>(static_cast<float>(x) * static_cast<float>(MLX_WIDTH) / static_cast<float>(outputWidth));
    int sensorY = static_cast<int>(static_cast<float>(y) * static_cast<float>(MLX_HEIGHT) / static_cast<float>(outputHeight));
    sensorX = clampInt(sensorX, 0, MLX_WIDTH - 1);
    sensorY = clampInt(sensorY, 0, MLX_HEIGHT - 1);
    return sensorY * MLX_WIDTH + sensorX;
}

void thermalDrawCrosshair(uint16_t* buffer, int width, int height, int x, int y, int arm, uint16_t color) {
    if (buffer == nullptr) return;
    const uint16_t outline = 0x0000;
    const int gap = arm / 3;

    for (int offset = -arm; offset <= arm; offset++) {
        if (offset > -gap && offset < gap) continue;
        putPixel(buffer, width, height, x + offset, y - 1, outline);
        putPixel(buffer, width, height, x + offset, y + 1, outline);
        putPixel(buffer, width, height, x + offset, y, color);
        putPixel(buffer, width, height, x - 1, y + offset, outline);
        putPixel(buffer, width, height, x + 1, y + offset, outline);
        putPixel(buffer, width, height, x, y + offset, color);
    }
}

void thermalDrawRect(uint16_t* buffer, int width, int height, int x0, int y0, int x1, int y1, uint16_t color) {
    if (buffer == nullptr) return;
    if (x0 > x1) {
        const int swap = x0;
        x0 = x1;
        x1 = swap;
    }
    if (y0 > y1) {
        const int swap = y0;
        y0 = y1;
        y1 = swap;
    }

    for (int x = x0; x <= x1; x++) {
        putPixel(buffer, width, height, x, y0, color);
        putPixel(buffer, width, height, x, y1, color);
    }
    for (int y = y0; y <= y1; y++) {
        putPixel(buffer, width, height, x0, y, color);
        putPixel(buffer, width, height, x1, y, color);
    }
}

void thermalDrawMarker(uint16_t* buffer, int width, int height, int x, int y, int size, uint16_t color) {
    if (buffer == nullptr) return;
    const uint16_t outline = 0x0000;
    thermalDrawRect(buffer, width, height, x - size - 1, y - size - 1, x + size + 1, y + size + 1, outline);
    thermalDrawRect(buffer, width, height, x - size, y - size, x + size, y + size, color);
}
