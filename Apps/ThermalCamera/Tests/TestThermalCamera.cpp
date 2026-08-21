// Round-trip test of the MLX90640 temperature maths.
//
// A synthetic EEPROM is fed through the driver's parameter extraction, then a
// forward model derived from the datasheet computes the raw ADC word that a
// sensor would produce for a known object temperature. Pushing that word back
// through calculateTemperatures() must return the temperature we started from.
#include "Mlx90640.h"
#include "Palette.h"
#include "Snapshot.h"
#include "ThermalImage.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

void checkClose(float actual, float expected, float tolerance, const char* what) {
    if (std::fabs(actual - expected) > tolerance) {
        printf("FAIL: %s (got %.4f, expected %.4f)\n", what, actual, expected);
        failures++;
    }
}

// --- Synthetic EEPROM ------------------------------------------------------

constexpr float ALPHA_PTAT = 8.5f;
constexpr int V_PTAT_25 = 12000;
constexpr float KT_PTAT = 42.0f;
constexpr int GAIN_EE = 6552;

std::vector<uint16_t> makeEeprom() {
    std::vector<uint16_t> ee(MLX_EEPROM_WORDS, 0);

    ee[10] = 0x0000;             // device select clear (a real part), chess calibration
    ee[16] = 0x2211;             // alphaPTAT nibble 2, occ scales 2/1/1
    ee[17] = 0xF830;             // offsetRef = -1992
    ee[32] = 0x8221;             // alphaScale 8, acc scales 2/2/1
    ee[33] = 0x4E20;             // alphaRef
    ee[48] = static_cast<uint16_t>(GAIN_EE);
    ee[49] = static_cast<uint16_t>(V_PTAT_25);
    ee[50] = static_cast<uint16_t>((18u << 10) | 336u); // KvPTAT 18/4096, KtPTAT 336/8
    ee[51] = 0x9D89;             // kVdd = -99*32, vdd25 = (137-256)*32-8192
    ee[52] = 0x2222;             // all Kv nibbles = 2
    ee[53] = 0x0000;             // no interleave correction
    ee[54] = 0x2929;             // Kta row/column constants
    ee[55] = 0x2929;
    ee[56] = 0x2251;             // resolution 2, kvScale 2, ktaScale1 5, ktaScale2 1
    ee[57] = 0x00C8;             // cp alpha (about 8% of a pixel's alpha)
    ee[58] = 0x03B5;             // cp offset = -75
    ee[59] = 0x0229;             // cpKv, cpKta
    ee[60] = 0xF020;             // KsTa = -16/8192, tgc = 32/32
    ee[61] = 0x0202;             // ksTo[0], ksTo[1]
    ee[62] = 0x0202;             // ksTo[2], ksTo[3]
    ee[63] = 0x1C84;             // step 10, ct3 nibble 12, ct2 nibble 8, scale 4

    // Per-pixel words: offset, alpha and kta deviations that differ per pixel
    // but never set the outlier bit and are never zero.
    for (int i = 0; i < MLX_PIXELS; i++) {
        const uint32_t offsetBits = static_cast<uint32_t>((i * 7) % 64);
        const uint32_t alphaBits = static_cast<uint32_t>((i * 13) % 64);
        const uint32_t ktaBits = static_cast<uint32_t>((i * 3) % 8);
        ee[64 + i] = static_cast<uint16_t>((offsetBits << 10) | (alphaBits << 4) | (ktaBits << 1));
        if (ee[64 + i] == 0) ee[64 + i] = 0x0010;
    }
    return ee;
}

class FakeBus final : public MlxBus {
public:
    explicit FakeBus(std::vector<uint16_t> eeprom) : eeprom_(std::move(eeprom)) {}

    bool readWords(uint16_t startAddress, uint16_t* out, size_t count) override {
        if (startAddress == 0x2400) {
            for (size_t i = 0; i < count; i++) out[i] = eeprom_[i];
            return true;
        }
        for (size_t i = 0; i < count; i++) out[i] = 0;
        return true;
    }

    bool writeWord(uint16_t, uint16_t) override { return true; }

private:
    std::vector<uint16_t> eeprom_;
};

// --- Forward model ---------------------------------------------------------

uint16_t toRaw(float value) {
    int32_t rounded = static_cast<int32_t>(value >= 0.0f ? value + 0.5f : value - 0.5f);
    if (rounded < -32768) rounded = -32768;
    if (rounded > 32767) rounded = 32767;
    return static_cast<uint16_t>(rounded & 0xFFFF);
}

/** Fill the housekeeping words so the sensor reports the given Ta at VDD 3.3 V. */
void fillHousekeeping(uint16_t* frame, const MlxCalibration& p, float ta, int subPage) {
    frame[832] = 0x1800;                            // chess mode, 18-bit resolution
    frame[833] = static_cast<uint16_t>(subPage);
    frame[810] = toRaw(p.vdd25);                    // VDD exactly 3.3 V
    frame[778] = static_cast<uint16_t>(GAIN_EE);    // gain factor 1.0

    const float vPtatArt = p.vPTAT25 + p.KtPTAT * (ta - 25.0f);
    const float ptat = 2000.0f;
    const float ptatArt = ptat * (262144.0f / vPtatArt) - p.alphaPTAT * ptat;
    frame[800] = toRaw(ptat);
    frame[768] = toRaw(ptatArt);

    // Compensation pixels: a plain reading, the driver subtracts their offset.
    frame[776] = toRaw(120.0f);
    frame[808] = toRaw(120.0f);
}

float signExtend16(uint16_t value) {
    return static_cast<float>(static_cast<int16_t>(value));
}

/** Compute the raw pixel word that corresponds to @p objectTemperature. */
uint16_t forwardPixel(
    const uint16_t* frame,
    const MlxCalibration& p,
    int pixel,
    float ta,
    float objectTemperature,
    float emissivity
) {
    const float vdd = 3.3f;
    const float gain = 1.0f;

    const float taK = ta + 273.15f;
    const float ta4 = taK * taK * taK * taK;
    const float taTr = ta4; // reflected temperature equals ambient in this test

    float alphaCorrR[4];
    alphaCorrR[0] = 1.0f / (1.0f + p.ksTo[0] * 40.0f);
    alphaCorrR[1] = 1.0f;
    alphaCorrR[2] = 1.0f + p.ksTo[1] * p.ct[2];
    alphaCorrR[3] = alphaCorrR[2] * (1.0f + p.ksTo[2] * (p.ct[3] - p.ct[2]));

    int range;
    if (objectTemperature < p.ct[1]) {
        range = 0;
    } else if (objectTemperature < p.ct[2]) {
        range = 1;
    } else if (objectTemperature < p.ct[3]) {
        range = 2;
    } else {
        range = 3;
    }

    const float alphaCompensated = p.alpha[pixel] * (1.0f + p.KsTa * (ta - 25.0f));
    const float toK = objectTemperature + 273.15f;
    const float toK4 = toK * toK * toK * toK;

    // Invert the second pass of the driver's To calculation.
    float irData = (toK4 - taTr) * alphaCompensated * alphaCorrR[range] *
                   (1.0f + p.ksTo[range] * (objectTemperature - p.ct[range]));

    // Undo emissivity and the thermal gradient coefficient.
    irData *= emissivity;

    float irDataCP[2];
    const float ktaTaScale = 1.0f + p.cpKta * (ta - 25.0f);
    const float ktaVddScale = 1.0f + p.cpKv * (vdd - 3.3f);
    irDataCP[0] = signExtend16(frame[776]) * gain - p.cpOffset[0] * ktaTaScale * ktaVddScale;
    irDataCP[1] = signExtend16(frame[808]) * gain - p.cpOffset[1] * ktaTaScale * ktaVddScale;
    irData += p.tgc * irDataCP[static_cast<int>(frame[833] & 1)];

    // Undo the per-pixel offset correction.
    const int row = pixel / MLX_WIDTH;
    const int column = pixel % MLX_WIDTH;
    const int split = 2 * (row & 1) + (column & 1);
    irData += static_cast<float>(p.offset[pixel]) * (1.0f + p.kta[pixel] * (ta - 25.0f)) *
              (1.0f + p.kv[split] * (vdd - 3.3f));

    return toRaw(irData / gain);
}

/** Pixels belonging to a sub-page in chess mode. */
bool belongsToSubPage(int pixel, int subPage) {
    const int interleaved = pixel / 32 - (pixel / 64) * 2;
    return (interleaved ^ (pixel - (pixel / 2) * 2)) == subPage;
}

// --- Tests -----------------------------------------------------------------

void testCalibrationExtraction(const Mlx90640& sensor) {
    const MlxCalibration& p = sensor.getCalibration();

    checkClose(p.kVdd, -99.0f * 32.0f, 0.001f, "kVdd");
    checkClose(p.vdd25, (137.0f - 256.0f) * 32.0f - 8192.0f, 0.001f, "vdd25");
    checkClose(p.KvPTAT, 18.0f / 4096.0f, 1e-6f, "KvPTAT");
    checkClose(p.KtPTAT, KT_PTAT, 0.001f, "KtPTAT");
    checkClose(p.vPTAT25, static_cast<float>(V_PTAT_25), 0.001f, "vPTAT25");
    checkClose(p.alphaPTAT, ALPHA_PTAT, 0.001f, "alphaPTAT");
    checkClose(p.gainEE, static_cast<float>(GAIN_EE), 0.001f, "gainEE");
    checkClose(p.tgc, 1.0f, 0.001f, "tgc");
    checkClose(p.KsTa, -16.0f / 8192.0f, 1e-6f, "KsTa");
    check(p.resolutionEE == 2, "resolutionEE");
    check(p.calibrationModeEE == 0x80, "calibrationModeEE is chess");
    checkClose(p.ct[2], 80.0f, 0.001f, "ct[2]");
    checkClose(p.ct[3], 200.0f, 0.001f, "ct[3]");
    checkClose(p.ksTo[1], 2.0f / 4096.0f, 1e-8f, "ksTo[1]");
    checkClose(p.cpOffset[0], -75.0f, 0.001f, "cpOffset[0]");
    check(p.badPixelCount == 0, "no bad pixels flagged");
    check(p.alpha[0] > 0.0f, "alpha is positive");

    // Kv uses the row/column parity split from the datasheet.
    for (int i = 0; i < 4; i++) checkClose(p.kv[i], 2.0f / 4.0f, 1e-6f, "kv split");
}

void testTemperatureRoundTrip(const Mlx90640& sensor) {
    const MlxCalibration& p = sensor.getCalibration();
    const float ta = 30.0f;
    const float emissivity = 1.0f;

    // One target per range boundary the driver selects between.
    const float targets[] = {-20.0f, 5.0f, 25.0f, 37.0f, 60.0f, 120.0f, 250.0f};

    std::vector<float> result(MLX_PIXELS, 0.0f);
    std::vector<uint16_t> frame(MLX_FRAME_WORDS, 0);

    for (float target : targets) {
        for (int subPage = 0; subPage < 2; subPage++) {
            std::fill(frame.begin(), frame.end(), static_cast<uint16_t>(0));
            fillHousekeeping(frame.data(), p, ta, subPage);
            for (int pixel = 0; pixel < MLX_PIXELS; pixel++) {
                if (!belongsToSubPage(pixel, subPage)) continue;
                frame[pixel] = forwardPixel(frame.data(), p, pixel, ta, target, emissivity);
            }

            MlxFrameInfo info;
            sensor.calculateTemperatures(frame.data(), emissivity, ta, result.data(), info);

            checkClose(info.ambientTemperature, ta, 0.05f, "recovered ambient temperature");
            checkClose(info.supplyVoltage, 3.3f, 0.01f, "recovered supply voltage");
            check(info.subPage == subPage, "sub-page reported");

            float worst = 0.0f;
            for (int pixel = 0; pixel < MLX_PIXELS; pixel++) {
                if (!belongsToSubPage(pixel, subPage)) continue;
                const float error = std::fabs(result[pixel] - target);
                if (error > worst) worst = error;
            }
            char label[64];
            snprintf(label, sizeof(label), "round trip at %.0f C (sub-page %d)", static_cast<double>(target), subPage);
            // The raw word is quantised to an integer, which limits the accuracy.
            checkClose(worst, 0.0f, 0.6f, label);
        }
    }
}

void testEmissivityDirection(const Mlx90640& sensor) {
    const MlxCalibration& p = sensor.getCalibration();
    const float ta = 30.0f;
    const float target = 80.0f;

    std::vector<float> highEmissivity(MLX_PIXELS, 0.0f);
    std::vector<float> lowEmissivity(MLX_PIXELS, 0.0f);
    std::vector<uint16_t> frame(MLX_FRAME_WORDS, 0);

    fillHousekeeping(frame.data(), p, ta, 0);
    for (int pixel = 0; pixel < MLX_PIXELS; pixel++) {
        if (!belongsToSubPage(pixel, 0)) continue;
        frame[pixel] = forwardPixel(frame.data(), p, pixel, ta, target, 1.0f);
    }

    MlxFrameInfo info;
    sensor.calculateTemperatures(frame.data(), 1.0f, ta, highEmissivity.data(), info);
    sensor.calculateTemperatures(frame.data(), 0.7f, ta, lowEmissivity.data(), info);

    // Correcting for a less emissive surface must report a hotter object.
    check(lowEmissivity[0] > highEmissivity[0] + 1.0f, "lower emissivity reports a hotter object");
}

void testBadPixelRepair() {
    Mlx90640 sensor;
    auto eeprom = makeEeprom();
    eeprom[64 + 100] = 0; // dead pixel
    FakeBus bus(eeprom);
    check(sensor.begin(&bus), "begin() with one dead pixel");
    check(sensor.getCalibration().badPixelCount == 1, "one bad pixel detected");

    std::vector<float> frame(MLX_PIXELS, 20.0f);
    frame[100] = -273.0f;
    sensor.repairBadPixels(frame.data());
    checkClose(frame[100], 20.0f, 0.001f, "dead pixel replaced by its neighbours");
}

void testImageHelpers() {
    std::vector<float> frame(MLX_PIXELS, 25.0f);
    frame[MLX_WIDTH * 10 + 5] = 90.0f;
    frame[MLX_WIDTH * 20 + 30] = 5.0f;

    FrameStats stats;
    thermalComputeStats(frame.data(), stats);
    checkClose(stats.maximum, 90.0f, 0.001f, "frame maximum");
    checkClose(stats.minimum, 5.0f, 0.001f, "frame minimum");
    check(stats.maximumIndex == MLX_WIDTH * 10 + 5, "maximum index");
    check(stats.minimumIndex == MLX_WIDTH * 20 + 30, "minimum index");

    RegionStats region;
    thermalComputeRegionStats(frame.data(), 0, 0, 9, 9, region);
    checkClose(region.maximum, 25.0f, 0.001f, "region excludes the hot spot");
    check(region.pixelCount == 100, "region pixel count");

    checkClose(thermalSample(frame.data(), 5.0f, 10.0f), 90.0f, 0.001f, "sample hits the hot pixel");
    checkClose(thermalConvertUnit(100.0f, UNIT_FAHRENHEIT), 212.0f, 0.001f, "Celsius to Fahrenheit");
    checkClose(thermalConvertToCelsius(212.0f, UNIT_FAHRENHEIT), 100.0f, 0.001f, "Fahrenheit to Celsius");

    // Rendering: the hot pixel must land on the hot end of the palette, and the
    // mirrored image must place it on the opposite side.
    uint16_t palette[PALETTE_LEVELS];
    uint32_t paletteRgb[PALETTE_LEVELS];
    paletteBuild(PALETTE_IRON, palette, paletteRgb);
    check(palette[0] != palette[PALETTE_LEVELS - 1], "palette spans a range");

    constexpr int width = MLX_WIDTH * 4;
    constexpr int height = MLX_HEIGHT * 4;
    std::vector<uint16_t> image(static_cast<size_t>(width) * height, 0);
    std::vector<ColumnMap> map(width);

    RenderOptions options;
    options.palette = palette;
    options.rangeMinimum = 5.0f;
    options.rangeMaximum = 90.0f;
    options.interpolation = INTERP_NONE;

    thermalBuildColumnMap(width, false, map.data());
    thermalRender(frame.data(), image.data(), width, height, options, map.data());

    int x = 0;
    int y = 0;
    thermalSensorToOutput(MLX_WIDTH * 10 + 5, width, height, false, false, x, y);
    check(image[static_cast<size_t>(y) * width + x] == palette[PALETTE_LEVELS - 1], "hot pixel uses the hot colour");
    check(thermalOutputToSensor(x, y, width, height, false, false) == MLX_WIDTH * 10 + 5, "output maps back to sensor");

    options.mirror = true;
    thermalBuildColumnMap(width, true, map.data());
    thermalRender(frame.data(), image.data(), width, height, options, map.data());
    int mirroredX = 0;
    int mirroredY = 0;
    thermalSensorToOutput(MLX_WIDTH * 10 + 5, width, height, true, false, mirroredX, mirroredY);
    check(mirroredX != x, "mirroring moves the hot pixel");
    check(
        image[static_cast<size_t>(mirroredY) * width + mirroredX] == palette[PALETTE_LEVELS - 1],
        "hot pixel stays hot when mirrored"
    );

    // The alarm colour must win over the palette.
    options.mirror = false;
    thermalBuildColumnMap(width, false, map.data());
    options.alarmMode = ALARM_ABOVE;
    options.alarmHigh = 80.0f;
    options.alarmColor = 0x07E0;
    thermalRender(frame.data(), image.data(), width, height, options, map.data());
    check(image[static_cast<size_t>(y) * width + x] == 0x07E0, "alarm colour overrides the palette");

    // Drawing helpers must stay inside the buffer.
    thermalDrawCrosshair(image.data(), width, height, 0, 0, 20, 0xFFFF);
    thermalDrawRect(image.data(), width, height, -5, -5, width + 5, height + 5, 0xFFFF);
    thermalDrawMarker(image.data(), width, height, width - 1, height - 1, 6, 0xFFFF);
}

void testTemporalFilter() {
    std::vector<float> accumulator(MLX_PIXELS, 0.0f);
    std::vector<float> frame(MLX_PIXELS, 10.0f);

    thermalTemporalFilter(accumulator.data(), frame.data(), 1.0f);
    checkClose(accumulator[0], 10.0f, 0.001f, "filter weight 1 passes the frame through");

    std::fill(accumulator.begin(), accumulator.end(), 0.0f);
    for (int i = 0; i < 200; i++) thermalTemporalFilter(accumulator.data(), frame.data(), 0.15f);
    checkClose(accumulator[0], 10.0f, 0.01f, "filter converges on the input");
}

void testBmpWriter() {
    constexpr int width = 8;
    constexpr int height = 4;
    uint16_t pixels[width * height];
    for (int i = 0; i < width * height; i++) pixels[i] = static_cast<uint16_t>(i * 137);

    const char* path = "/tmp/thermal_test.bmp";
    check(snapshotWriteBmp(path, pixels, width, height), "bitmap written");

    FILE* file = fopen(path, "rb");
    check(file != nullptr, "bitmap can be reopened");
    if (file == nullptr) return;

    uint8_t header[54];
    check(fread(header, 1, sizeof(header), file) == sizeof(header), "bitmap header read");
    check(header[0] == 'B' && header[1] == 'M', "bitmap magic");

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fclose(file);
    // 8 pixels * 3 bytes = 24, already a multiple of four, so no padding.
    check(size == 54 + width * 3 * height, "bitmap size matches the header");
    remove(path);
}

} // namespace

int main() {
    Mlx90640 sensor;
    FakeBus bus(makeEeprom());
    check(sensor.begin(&bus), "begin() succeeds on a valid EEPROM");
    check(sensor.isReady(), "sensor reports ready");

    testCalibrationExtraction(sensor);
    testTemperatureRoundTrip(sensor);
    testEmissivityDirection(sensor);
    testBadPixelRepair();
    testImageHelpers();
    testTemporalFilter();
    testBmpWriter();

    // A real MLX90640 has the device select bit of EE[10] clear. A set bit is
    // recorded but must not stop the sensor from being used, because the
    // calibration checks are the stronger evidence.
    Mlx90640 otherPart;
    auto deviceSelectSet = makeEeprom();
    deviceSelectSet[10] = 0x0040;
    FakeBus deviceSelectBus(deviceSelectSet);
    check(otherPart.begin(&deviceSelectBus), "a set device select bit still initialises");
    check(otherPart.hasDeviceSelectMismatch(), "the device select mismatch is reported");
    check(!sensor.hasDeviceSelectMismatch(), "a real part reports no mismatch");
    check(sensor.getInitStatus() == MLX_INIT_OK, "a good EEPROM reports MLX_INIT_OK");

    // An EEPROM that carries no calibration at all must be refused.
    Mlx90640 rejected;
    std::vector<uint16_t> blank(MLX_EEPROM_WORDS, 0);
    FakeBus blankBus(blank);
    check(!rejected.begin(&blankBus), "a blank EEPROM is rejected");
    check(rejected.getInitStatus() == MLX_INIT_BAD_CALIBRATION, "blank EEPROM reports a bad calibration");

    // A transport that cannot read must be distinguishable from a bad sensor.
    Mlx90640 unreachable;
    check(!unreachable.begin(nullptr), "a missing bus is refused");
    check(unreachable.getInitStatus() == MLX_INIT_NO_BUS, "a missing bus reports MLX_INIT_NO_BUS");

    if (failures == 0) {
        printf("All checks passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
