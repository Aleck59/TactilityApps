/**
 * @file Mlx90640.h
 * @brief MLX90640 32x24 far-infrared thermal sensor array driver.
 *
 * The calibration extraction and temperature calculation follow the MLX90640
 * datasheet (rev. 12) chapter 11 "Measurement and calibration data" step by step.
 * Calibration constants are kept as floats instead of the packed integer form
 * that the reference implementation uses, because we have RAM to spare and the
 * unpacked form avoids a second rounding step per pixel.
 */
#pragma once

#include <cstddef>
#include <cstdint>

/** Sensor geometry */
static constexpr int MLX_WIDTH = 32;
static constexpr int MLX_HEIGHT = 24;
static constexpr int MLX_PIXELS = MLX_WIDTH * MLX_HEIGHT;

/** Number of 16-bit words in the sensor EEPROM (0x2400..0x273F) */
static constexpr int MLX_EEPROM_WORDS = 832;
/** Number of 16-bit words in the sensor RAM (0x0400..0x073F) */
static constexpr int MLX_RAM_WORDS = 832;

/** Number of 16-bit words in a raw frame: 832 RAM words + control + status */
static constexpr int MLX_FRAME_WORDS = 834;

/** Default 7-bit I2C address (A0 tied low) */
static constexpr uint8_t MLX_DEFAULT_ADDRESS = 0x33;

/** Sensor refresh rate. Note that a full image needs two sub-pages, so the
 *  effective image rate is half of these values. */
enum MlxRefreshRate : uint8_t {
    MLX_RATE_0_5HZ = 0,
    MLX_RATE_1HZ = 1,
    MLX_RATE_2HZ = 2,
    MLX_RATE_4HZ = 3,
    MLX_RATE_8HZ = 4,
    MLX_RATE_16HZ = 5,
    MLX_RATE_32HZ = 6,
    MLX_RATE_64HZ = 7
};

/** ADC resolution used for the measurement */
enum MlxResolution : uint8_t {
    MLX_RESOLUTION_16BIT = 0,
    MLX_RESOLUTION_17BIT = 1,
    MLX_RESOLUTION_18BIT = 2,
    MLX_RESOLUTION_19BIT = 3
};

/** Why begin() failed, so the interface can name the real cause. */
enum MlxInitStatus : uint8_t {
    MLX_INIT_OK = 0,
    /** No transport was given. */
    MLX_INIT_NO_BUS,
    /** The EEPROM transfer did not complete. */
    MLX_INIT_READ_FAILED,
    /** The scratch buffer could not be allocated. */
    MLX_INIT_OUT_OF_MEMORY,
    /** The EEPROM was read but its contents are not a usable calibration. */
    MLX_INIT_BAD_CALIBRATION,
    /** begin() has not run yet. */
    MLX_INIT_NOT_STARTED
};

/** Human readable form of MlxInitStatus. */
const char* mlxInitStatusName(MlxInitStatus status);

/** Read-out pattern. Chess is the factory default and gives the best image quality. */
enum MlxPattern : uint8_t {
    MLX_PATTERN_INTERLEAVED = 0,
    MLX_PATTERN_CHESS = 1
};

/**
 * @brief Transport abstraction so the driver can be unit-tested off-target.
 *
 * All transfers use the sensor's 16-bit register addressing and big-endian words.
 */
class MlxBus {
public:
    /** Read @p count 16-bit words starting at @p startAddress. */
    virtual bool readWords(uint16_t startAddress, uint16_t* out, size_t count) = 0;

    /** Write a single 16-bit register. */
    virtual bool writeWord(uint16_t address, uint16_t value) = 0;

protected:
    /** Non-virtual on purpose: implementations are held by value, never owned
     *  through this interface. A virtual destructor would put a deleting
     *  destructor in the vtable, and the deleting destructor calls a form of
     *  operator delete that the ELF loader's symbol table does not export. */
    ~MlxBus() = default;
};

/** Unpacked calibration data, roughly 11 kB. */
struct MlxCalibration {
    float kVdd = 0.0f;
    float vdd25 = 0.0f;
    float KvPTAT = 0.0f;
    float KtPTAT = 0.0f;
    float vPTAT25 = 0.0f;
    float alphaPTAT = 0.0f;
    float gainEE = 0.0f;
    float tgc = 0.0f;
    float KsTa = 0.0f;
    float ksTo[5] = {};
    float ct[5] = {};
    float cpAlpha[2] = {};
    float cpOffset[2] = {};
    float cpKta = 0.0f;
    float cpKv = 0.0f;
    float ilChessC[3] = {};
    uint8_t resolutionEE = 0;
    uint8_t calibrationModeEE = 0;

    float alpha[MLX_PIXELS] = {};
    int16_t offset[MLX_PIXELS] = {};
    float kta[MLX_PIXELS] = {};
    float kv[4] = {};

    /** Indices of pixels the factory marked as broken or as outliers. */
    uint16_t badPixels[MLX_PIXELS] = {};
    uint16_t badPixelCount = 0;
};

/** Result of a successful frame conversion. */
struct MlxFrameInfo {
    /** Sub-page that produced this data (0 or 1) */
    int subPage = 0;
    /** Sensor die temperature in degrees Celsius */
    float ambientTemperature = 0.0f;
    /** Supply voltage as measured by the sensor */
    float supplyVoltage = 0.0f;
};

class Mlx90640 {
public:
    /**
     * @brief Read and unpack the EEPROM. Must be called once before reading frames.
     * @param bus the transport to use (kept by reference, must outlive this object)
     * @param address the 7-bit I2C address of the sensor
     */
    bool begin(MlxBus* bus, uint8_t address = MLX_DEFAULT_ADDRESS);

    /** @return true when begin() succeeded */
    bool isReady() const { return ready_; }

    /** Drop the current calibration so the next loop iteration re-detects the sensor. */
    void reset() {
        ready_ = false;
        errorCount_ = 0;
        initStatus_ = MLX_INIT_NOT_STARTED;
    }

    bool setRefreshRate(MlxRefreshRate rate);
    bool getRefreshRate(MlxRefreshRate& out);
    bool setResolution(MlxResolution resolution);
    bool setPattern(MlxPattern pattern);

    /**
     * @brief Read one sub-page of raw sensor data.
     * @param[out] frameData buffer of MLX_FRAME_WORDS words
     * @return true when a complete sub-page was read
     */
    bool readRawFrame(uint16_t* frameData);

    /**
     * @brief Convert raw sub-page data into object temperatures.
     *
     * Only the pixels that belong to the sub-page contained in @p frameData are
     * written, so @p result must be persistent between calls to build a full image.
     *
     * @param[in] frameData raw data from readRawFrame()
     * @param[in] emissivity emissivity of the observed surface (0.01 .. 1.0)
     * @param[in] reflectedTemperature apparent temperature of the surroundings in degrees Celsius
     * @param[out] result MLX_PIXELS temperatures in degrees Celsius
     * @param[out] info per-frame housekeeping values
     */
    void calculateTemperatures(
        const uint16_t* frameData,
        float emissivity,
        float reflectedTemperature,
        float* result,
        MlxFrameInfo& info
    ) const;

    /** Replace factory-marked bad pixels by the average of their valid neighbours. */
    void repairBadPixels(float* frame) const;

    const MlxCalibration& getCalibration() const { return calibration_; }

    /** Ambient (die) temperature of the last converted frame. */
    float getAmbientTemperature() const { return lastAmbientTemperature_; }

    /** Consecutive failed transfers since the last successful frame. */
    uint32_t getErrorCount() const { return errorCount_; }

    /** Why the last begin() call ended the way it did. */
    MlxInitStatus getInitStatus() const { return initStatus_; }

    /** True when EE[10] did not look like an MLX90640 device select word. */
    bool hasDeviceSelectMismatch() const { return deviceSelectMismatch_; }

private:
    MlxBus* bus_ = nullptr;
    uint8_t address_ = MLX_DEFAULT_ADDRESS;
    bool ready_ = false;
    MlxInitStatus initStatus_ = MLX_INIT_NOT_STARTED;
    bool deviceSelectMismatch_ = false;
    mutable float lastAmbientTemperature_ = 25.0f;
    uint32_t errorCount_ = 0;
    MlxCalibration calibration_;

    bool extractParameters(const uint16_t* eeData);
    float getVdd(const uint16_t* frameData) const;
    float getTa(const uint16_t* frameData) const;
};
