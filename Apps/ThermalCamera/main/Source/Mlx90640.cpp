#include "Mlx90640.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint16_t REG_STATUS = 0x8000;
constexpr uint16_t REG_CONTROL1 = 0x800D;
constexpr uint16_t RAM_START = 0x0400;
constexpr uint16_t EEPROM_START = 0x2400;

/** Value written to the status register to acknowledge a frame (datasheet 12.3) */
constexpr uint16_t STATUS_ACK = 0x0030;

/** Sign-extend the lowest @p bits bits of @p value. */
inline int32_t signExtend(uint32_t value, int bits) {
    const uint32_t mask = 1u << (bits - 1);
    const uint32_t limited = value & ((1u << bits) - 1u);
    return static_cast<int32_t>((limited ^ mask) - mask);
}

/** Two raised to @p exponent. The EEPROM scales reach 45, well past what a
 *  32-bit shift can express, so this goes through the exponent directly. */
inline float pow2f(int exponent) {
    return ldexpf(1.0f, exponent);
}

/** The sub-page a pixel belongs to, for both read-out patterns. */
inline int pixelPattern(int pixelNumber, bool chess) {
    const int interleaved = pixelNumber / 32 - (pixelNumber / 64) * 2;
    if (!chess) return interleaved;
    return interleaved ^ (pixelNumber - (pixelNumber / 2) * 2);
}

} // namespace

bool Mlx90640::begin(MlxBus* bus, uint8_t address) {
    ready_ = false;
    bus_ = bus;
    address_ = address;
    if (bus_ == nullptr) return false;

    // The EEPROM image is 1664 bytes, too much for the stack, and it is only
    // needed here. malloc rather than new: Tactility's ELF loader resolves
    // operator new(size_t) but not the array or nothrow forms.
    auto* eeData = static_cast<uint16_t*>(malloc(MLX_EEPROM_WORDS * sizeof(uint16_t)));
    if (eeData == nullptr) return false;

    bool success = bus_->readWords(EEPROM_START, eeData, MLX_EEPROM_WORDS);
    if (success) {
        // Datasheet: EE[10] bit 6 identifies a valid MLX90640 device select word.
        success = (eeData[10] & 0x0040) != 0;
    }
    if (success) {
        success = extractParameters(eeData);
    }

    free(eeData);
    ready_ = success;
    return success;
}

bool Mlx90640::extractParameters(const uint16_t* ee) {
    MlxCalibration& p = calibration_;

    // --- VDD ---------------------------------------------------------------
    p.kVdd = static_cast<float>(signExtend((ee[51] & 0xFF00) >> 8, 8) * 32);
    p.vdd25 = static_cast<float>((static_cast<int>(ee[51] & 0x00FF) - 256) * 32 - 8192);
    if (p.kVdd == 0.0f) return false;

    // --- PTAT (die temperature sensor) -------------------------------------
    p.KvPTAT = static_cast<float>(signExtend((ee[50] & 0xFC00) >> 10, 6)) / 4096.0f;
    p.KtPTAT = static_cast<float>(signExtend(ee[50] & 0x03FF, 10)) / 8.0f;
    p.vPTAT25 = static_cast<float>(ee[49]);
    p.alphaPTAT = static_cast<float>((ee[16] & 0xF000) >> 12) / 4.0f + 8.0f;
    if (p.KtPTAT == 0.0f) return false;

    // --- Gain --------------------------------------------------------------
    p.gainEE = static_cast<float>(signExtend(ee[48], 16));
    if (p.gainEE == 0.0f) return false;

    // --- Sensitivity/temperature corrections --------------------------------
    p.tgc = static_cast<float>(signExtend(ee[60] & 0x00FF, 8)) / 32.0f;
    p.resolutionEE = static_cast<uint8_t>((ee[56] & 0x3000) >> 12);
    p.KsTa = static_cast<float>(signExtend((ee[60] & 0xFF00) >> 8, 8)) / 8192.0f;

    // --- Temperature range corner points and their slopes -------------------
    const int step = static_cast<int>((ee[63] & 0x3000) >> 12) * 10;
    p.ct[0] = -40.0f;
    p.ct[1] = 0.0f;
    p.ct[2] = static_cast<float>(static_cast<int>((ee[63] & 0x00F0) >> 4) * step);
    p.ct[3] = p.ct[2] + static_cast<float>(static_cast<int>((ee[63] & 0x0F00) >> 8) * step);
    p.ct[4] = 400.0f;

    const float ksToScale = pow2f(static_cast<int>(ee[63] & 0x000F) + 8);
    p.ksTo[0] = static_cast<float>(signExtend(ee[61] & 0x00FF, 8)) / ksToScale;
    p.ksTo[1] = static_cast<float>(signExtend((ee[61] & 0xFF00) >> 8, 8)) / ksToScale;
    p.ksTo[2] = static_cast<float>(signExtend(ee[62] & 0x00FF, 8)) / ksToScale;
    p.ksTo[3] = static_cast<float>(signExtend((ee[62] & 0xFF00) >> 8, 8)) / ksToScale;
    p.ksTo[4] = -0.0002f;

    // --- Compensation pixel -------------------------------------------------
    const int cpAlphaScale = static_cast<int>((ee[32] & 0xF000) >> 12) + 27;
    const int ktaScale1 = static_cast<int>((ee[56] & 0x00F0) >> 4) + 8;
    const int ktaScale2 = static_cast<int>(ee[56] & 0x000F);
    const int kvScale = static_cast<int>((ee[56] & 0x0F00) >> 8);

    p.cpOffset[0] = static_cast<float>(signExtend(ee[58] & 0x03FF, 10));
    p.cpOffset[1] = p.cpOffset[0] + static_cast<float>(signExtend((ee[58] & 0xFC00) >> 10, 6));
    p.cpAlpha[0] = static_cast<float>(signExtend(ee[57] & 0x03FF, 10)) / pow2f(cpAlphaScale);
    p.cpAlpha[1] = p.cpAlpha[0] * (1.0f + static_cast<float>(signExtend((ee[57] & 0xFC00) >> 10, 6)) / 128.0f);
    p.cpKta = static_cast<float>(signExtend(ee[59] & 0x00FF, 8)) / pow2f(ktaScale1);
    p.cpKv = static_cast<float>(signExtend((ee[59] & 0xFF00) >> 8, 8)) / pow2f(kvScale);

    // --- Interleaved-mode chess correction ----------------------------------
    p.calibrationModeEE = static_cast<uint8_t>((((ee[10] & 0x0800) >> 4) ^ 0x80) & 0xFF);
    p.ilChessC[0] = static_cast<float>(signExtend(ee[53] & 0x003F, 6)) / 16.0f;
    p.ilChessC[1] = static_cast<float>(signExtend((ee[53] & 0x07C0) >> 6, 5)) / 2.0f;
    p.ilChessC[2] = static_cast<float>(signExtend((ee[53] & 0xF800) >> 11, 5)) / 8.0f;

    // --- Per-pixel offset ---------------------------------------------------
    const int occRemScale = static_cast<int>(ee[16] & 0x000F);
    const int occColumnScale = static_cast<int>((ee[16] & 0x00F0) >> 4);
    const int occRowScale = static_cast<int>((ee[16] & 0x0F00) >> 8);
    const int offsetRef = signExtend(ee[17], 16);

    int occRow[MLX_HEIGHT];
    int occColumn[MLX_WIDTH];
    for (int i = 0; i < 6; i++) {
        const int base = i * 4;
        occRow[base + 0] = signExtend(ee[18 + i] & 0x000F, 4);
        occRow[base + 1] = signExtend((ee[18 + i] & 0x00F0) >> 4, 4);
        occRow[base + 2] = signExtend((ee[18 + i] & 0x0F00) >> 8, 4);
        occRow[base + 3] = signExtend((ee[18 + i] & 0xF000) >> 12, 4);
    }
    for (int i = 0; i < 8; i++) {
        const int base = i * 4;
        occColumn[base + 0] = signExtend(ee[24 + i] & 0x000F, 4);
        occColumn[base + 1] = signExtend((ee[24 + i] & 0x00F0) >> 4, 4);
        occColumn[base + 2] = signExtend((ee[24 + i] & 0x0F00) >> 8, 4);
        occColumn[base + 3] = signExtend((ee[24 + i] & 0xF000) >> 12, 4);
    }

    // --- Per-pixel sensitivity ----------------------------------------------
    const int accRemScale = static_cast<int>(ee[32] & 0x000F);
    const int accColumnScale = static_cast<int>((ee[32] & 0x00F0) >> 4);
    const int accRowScale = static_cast<int>((ee[32] & 0x0F00) >> 8);
    const int alphaScale = static_cast<int>((ee[32] & 0xF000) >> 12) + 30;
    const float alphaRef = static_cast<float>(ee[33]);

    int accRow[MLX_HEIGHT];
    int accColumn[MLX_WIDTH];
    for (int i = 0; i < 6; i++) {
        const int base = i * 4;
        accRow[base + 0] = signExtend(ee[34 + i] & 0x000F, 4);
        accRow[base + 1] = signExtend((ee[34 + i] & 0x00F0) >> 4, 4);
        accRow[base + 2] = signExtend((ee[34 + i] & 0x0F00) >> 8, 4);
        accRow[base + 3] = signExtend((ee[34 + i] & 0xF000) >> 12, 4);
    }
    for (int i = 0; i < 8; i++) {
        const int base = i * 4;
        accColumn[base + 0] = signExtend(ee[40 + i] & 0x000F, 4);
        accColumn[base + 1] = signExtend((ee[40 + i] & 0x00F0) >> 4, 4);
        accColumn[base + 2] = signExtend((ee[40 + i] & 0x0F00) >> 8, 4);
        accColumn[base + 3] = signExtend((ee[40 + i] & 0xF000) >> 12, 4);
    }

    // --- Per-pixel Kta ------------------------------------------------------
    float ktaRC[4];
    ktaRC[0] = static_cast<float>(signExtend((ee[54] & 0xFF00) >> 8, 8)); // row odd, column odd
    ktaRC[2] = static_cast<float>(signExtend(ee[54] & 0x00FF, 8));        // row even, column odd
    ktaRC[1] = static_cast<float>(signExtend((ee[55] & 0xFF00) >> 8, 8)); // row odd, column even
    ktaRC[3] = static_cast<float>(signExtend(ee[55] & 0x00FF, 8));        // row even, column even

    // --- Per-pixel Kv (four values, selected by the row/column parity) ------
    p.kv[0] = static_cast<float>(signExtend((ee[52] & 0xF000) >> 12, 4)) / pow2f(kvScale);
    p.kv[1] = static_cast<float>(signExtend((ee[52] & 0x00F0) >> 4, 4)) / pow2f(kvScale);
    p.kv[2] = static_cast<float>(signExtend((ee[52] & 0x0F00) >> 8, 4)) / pow2f(kvScale);
    p.kv[3] = static_cast<float>(signExtend(ee[52] & 0x000F, 4)) / pow2f(kvScale);

    const float alphaScaleDiv = pow2f(alphaScale);
    const float ktaScale1Div = pow2f(ktaScale1);
    const float tgcAlphaCorrection = p.tgc * (p.cpAlpha[0] + p.cpAlpha[1]) / 2.0f;

    p.badPixelCount = 0;
    for (int row = 0; row < MLX_HEIGHT; row++) {
        for (int column = 0; column < MLX_WIDTH; column++) {
            const int index = row * MLX_WIDTH + column;
            const uint16_t word = ee[64 + index];

            const int offsetTemp = signExtend((word & 0xFC00) >> 10, 6) * (1 << occRemScale);
            p.offset[index] = static_cast<int16_t>(
                offsetRef + (occRow[row] << occRowScale) + (occColumn[column] << occColumnScale) + offsetTemp
            );

            const float alphaTemp = static_cast<float>(signExtend((word & 0x03F0) >> 4, 6) * (1 << accRemScale));
            float alpha = (alphaRef + static_cast<float>(accRow[row] << accRowScale) +
                           static_cast<float>(accColumn[column] << accColumnScale) + alphaTemp) /
                          alphaScaleDiv;
            alpha -= tgcAlphaCorrection;
            p.alpha[index] = alpha;

            // Kta uses the same row/column parity split as Kv.
            const int split = 2 * (row & 1) + (column & 1);
            const float ktaTemp = static_cast<float>(signExtend((word & 0x000E) >> 1, 3) * (1 << ktaScale2));
            p.kta[index] = (ktaRC[split] + ktaTemp) / ktaScale1Div;

            // A zero word means a dead pixel; bit 0 marks a factory outlier.
            if (word == 0 || (word & 0x0001) != 0) {
                p.badPixels[p.badPixelCount++] = static_cast<uint16_t>(index);
            }
        }
    }

    // A sensor with a broken calibration would produce garbage rather than an
    // obviously wrong image, so refuse it outright.
    return p.badPixelCount < (MLX_PIXELS / 4);
}

bool Mlx90640::setRefreshRate(MlxRefreshRate rate) {
    if (bus_ == nullptr) return false;
    uint16_t control = 0;
    if (!bus_->readWords(REG_CONTROL1, &control, 1)) return false;
    control = static_cast<uint16_t>((control & 0xFC7F) | (static_cast<uint16_t>(rate & 0x07) << 7));
    return bus_->writeWord(REG_CONTROL1, control);
}

bool Mlx90640::getRefreshRate(MlxRefreshRate& out) {
    if (bus_ == nullptr) return false;
    uint16_t control = 0;
    if (!bus_->readWords(REG_CONTROL1, &control, 1)) return false;
    out = static_cast<MlxRefreshRate>((control >> 7) & 0x07);
    return true;
}

bool Mlx90640::setResolution(MlxResolution resolution) {
    if (bus_ == nullptr) return false;
    uint16_t control = 0;
    if (!bus_->readWords(REG_CONTROL1, &control, 1)) return false;
    control = static_cast<uint16_t>((control & 0xF3FF) | (static_cast<uint16_t>(resolution & 0x03) << 10));
    return bus_->writeWord(REG_CONTROL1, control);
}

bool Mlx90640::setPattern(MlxPattern pattern) {
    if (bus_ == nullptr) return false;
    uint16_t control = 0;
    if (!bus_->readWords(REG_CONTROL1, &control, 1)) return false;
    if (pattern == MLX_PATTERN_CHESS) {
        control = static_cast<uint16_t>(control | 0x1000);
    } else {
        control = static_cast<uint16_t>(control & ~0x1000);
    }
    return bus_->writeWord(REG_CONTROL1, control);
}

bool Mlx90640::readRawFrame(uint16_t* frameData) {
    if (bus_ == nullptr || frameData == nullptr) return false;

    uint16_t status = 0;
    if (!bus_->readWords(REG_STATUS, &status, 1)) {
        errorCount_++;
        return false;
    }
    // Bit 3 is set by the sensor once a sub-page finished converting. Reading
    // before that would return the previous sub-page again.
    if ((status & 0x0008) == 0) return false;

    if (!bus_->writeWord(REG_STATUS, STATUS_ACK)) {
        errorCount_++;
        return false;
    }
    if (!bus_->readWords(RAM_START, frameData, MLX_RAM_WORDS)) {
        errorCount_++;
        return false;
    }

    uint16_t statusAfter = 0;
    if (!bus_->readWords(REG_STATUS, &statusAfter, 1)) {
        errorCount_++;
        return false;
    }
    // The sensor finished a new sub-page while we were reading, so the data we
    // just fetched is a mix of two sub-pages. Drop it instead of showing tears.
    if ((statusAfter & 0x0008) != 0) return false;

    uint16_t control = 0;
    if (!bus_->readWords(REG_CONTROL1, &control, 1)) {
        errorCount_++;
        return false;
    }

    frameData[832] = control;
    frameData[833] = static_cast<uint16_t>(status & 0x0001);
    errorCount_ = 0;
    return true;
}

float Mlx90640::getVdd(const uint16_t* frameData) const {
    float vdd = static_cast<float>(signExtend(frameData[810], 16));
    const int resolutionRam = static_cast<int>((frameData[832] & 0x0C00) >> 10);
    const float resolutionCorrection = pow2f(static_cast<int>(calibration_.resolutionEE) - resolutionRam);
    return (resolutionCorrection * vdd - calibration_.vdd25) / calibration_.kVdd + 3.3f;
}

float Mlx90640::getTa(const uint16_t* frameData) const {
    const float vdd = getVdd(frameData);
    const float ptat = static_cast<float>(signExtend(frameData[800], 16));
    const float ptatArt = static_cast<float>(signExtend(frameData[768], 16));

    const float denominator = ptat * calibration_.alphaPTAT + ptatArt;
    if (denominator == 0.0f) return lastAmbientTemperature_;

    const float vPtatArt = (ptat / denominator) * 262144.0f; // 2^18
    return (vPtatArt / (1.0f + calibration_.KvPTAT * (vdd - 3.3f)) - calibration_.vPTAT25) /
               calibration_.KtPTAT +
           25.0f;
}

void Mlx90640::calculateTemperatures(
    const uint16_t* frameData,
    float emissivity,
    float reflectedTemperature,
    float* result,
    MlxFrameInfo& info
) const {
    const MlxCalibration& p = calibration_;

    if (emissivity < 0.01f) emissivity = 0.01f;
    if (emissivity > 1.0f) emissivity = 1.0f;

    const int subPage = static_cast<int>(frameData[833] & 0x0001);
    const float vdd = getVdd(frameData);
    const float ta = getTa(frameData);
    lastAmbientTemperature_ = ta;

    info.subPage = subPage;
    info.ambientTemperature = ta;
    info.supplyVoltage = vdd;

    const float taK = ta + 273.15f;
    const float ta4 = taK * taK * taK * taK;
    const float trK = reflectedTemperature + 273.15f;
    const float tr4 = trK * trK * trK * trK;
    // Apparent radiation of the surroundings that the object reflects towards us.
    const float taTr = tr4 - (tr4 - ta4) / emissivity;

    float alphaCorrR[4];
    alphaCorrR[0] = 1.0f / (1.0f + p.ksTo[0] * 40.0f);
    alphaCorrR[1] = 1.0f;
    alphaCorrR[2] = 1.0f + p.ksTo[1] * p.ct[2];
    alphaCorrR[3] = alphaCorrR[2] * (1.0f + p.ksTo[2] * (p.ct[3] - p.ct[2]));

    float gain = static_cast<float>(signExtend(frameData[778], 16));
    if (gain == 0.0f) return;
    gain = p.gainEE / gain;

    // Control register bit 12 selects chess mode; calibrationModeEE holds the
    // mode the factory calibration was taken in, in the same 0x80/0x00 encoding.
    const uint8_t mode = static_cast<uint8_t>((frameData[832] & 0x1000) >> 5);
    const bool chess = mode != 0;
    const float ktaVddScale = (1.0f + p.cpKv * (vdd - 3.3f));
    const float ktaTaScale = (1.0f + p.cpKta * (ta - 25.0f));

    float irDataCP[2];
    irDataCP[0] = static_cast<float>(signExtend(frameData[776], 16)) * gain;
    irDataCP[1] = static_cast<float>(signExtend(frameData[808], 16)) * gain;
    irDataCP[0] -= p.cpOffset[0] * ktaTaScale * ktaVddScale;
    if (mode == p.calibrationModeEE) {
        irDataCP[1] -= p.cpOffset[1] * ktaTaScale * ktaVddScale;
    } else {
        irDataCP[1] -= (p.cpOffset[1] + p.ilChessC[0]) * ktaTaScale * ktaVddScale;
    }

    for (int pixel = 0; pixel < MLX_PIXELS; pixel++) {
        if (pixelPattern(pixel, chess) != subPage) continue;

        const int row = pixel / MLX_WIDTH;
        const int column = pixel % MLX_WIDTH;
        const int split = 2 * (row & 1) + (column & 1);

        float irData = static_cast<float>(signExtend(frameData[pixel], 16)) * gain;
        irData -= static_cast<float>(p.offset[pixel]) * (1.0f + p.kta[pixel] * (ta - 25.0f)) *
                  (1.0f + p.kv[split] * (vdd - 3.3f));

        if (mode != p.calibrationModeEE) {
            // Reading in a different pattern than the calibration used needs the
            // interleaved/chess cross-correction from the EEPROM.
            const int ilPattern = pixel / 32 - (pixel / 64) * 2;
            const int conversionPattern =
                ((pixel + 2) / 4 - (pixel + 3) / 4 + (pixel + 1) / 4 - pixel / 4) * (1 - 2 * ilPattern);
            irData += p.ilChessC[2] * static_cast<float>(2 * ilPattern - 1) -
                      p.ilChessC[1] * static_cast<float>(conversionPattern);
        }

        irData -= p.tgc * irDataCP[subPage];
        irData /= emissivity;

        const float alphaCompensated = p.alpha[pixel] * (1.0f + p.KsTa * (ta - 25.0f));
        if (alphaCompensated <= 0.0f) {
            result[pixel] = ta;
            continue;
        }

        // First pass: temperature assuming the mid range slope, used to pick the
        // range that the second pass corrects with.
        float sx = alphaCompensated * alphaCompensated * alphaCompensated * (irData + alphaCompensated * taTr);
        sx = sqrtf(sqrtf(fabsf(sx))) * p.ksTo[1];

        float divisor = alphaCompensated * (1.0f - p.ksTo[1] * 273.15f) + sx;
        if (divisor == 0.0f) {
            result[pixel] = ta;
            continue;
        }
        float toK4 = irData / divisor + taTr;
        if (toK4 < 0.0f) toK4 = 0.0f;
        float to = sqrtf(sqrtf(toK4)) - 273.15f;

        int range;
        if (to < p.ct[1]) {
            range = 0;
        } else if (to < p.ct[2]) {
            range = 1;
        } else if (to < p.ct[3]) {
            range = 2;
        } else {
            range = 3;
        }

        divisor = alphaCompensated * alphaCorrR[range] * (1.0f + p.ksTo[range] * (to - p.ct[range]));
        if (divisor == 0.0f) {
            result[pixel] = to;
            continue;
        }
        toK4 = irData / divisor + taTr;
        if (toK4 < 0.0f) toK4 = 0.0f;
        result[pixel] = sqrtf(sqrtf(toK4)) - 273.15f;
    }
}

void Mlx90640::repairBadPixels(float* frame) const {
    for (uint16_t i = 0; i < calibration_.badPixelCount; i++) {
        const int index = calibration_.badPixels[i];
        const int row = index / MLX_WIDTH;
        const int column = index % MLX_WIDTH;

        float sum = 0.0f;
        int count = 0;
        // Only the four direct neighbours: in chess mode the diagonals belong to
        // the same sub-page and are more likely to be stale.
        const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const auto& offset : offsets) {
            const int nr = row + offset[0];
            const int nc = column + offset[1];
            if (nr < 0 || nr >= MLX_HEIGHT || nc < 0 || nc >= MLX_WIDTH) continue;

            const int neighbour = nr * MLX_WIDTH + nc;
            bool neighbourIsBad = false;
            for (uint16_t j = 0; j < calibration_.badPixelCount; j++) {
                if (calibration_.badPixels[j] == neighbour) {
                    neighbourIsBad = true;
                    break;
                }
            }
            if (neighbourIsBad) continue;

            sum += frame[neighbour];
            count++;
        }

        if (count > 0) frame[index] = sum / static_cast<float>(count);
    }
}
