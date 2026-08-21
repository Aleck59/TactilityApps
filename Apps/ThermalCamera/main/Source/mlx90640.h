#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MLX90640 I2C default address */
#define MLX90640_ADDR 0x33

/** Sensor resolution: 32x24 pixels */
#define MLX90640_WIDTH  32
#define MLX90640_HEIGHT 24
#define MLX90640_PIXELS (MLX90640_WIDTH * MLX90640_HEIGHT)

/** Refresh rates */
typedef enum {
    MLX90640_RATE_0_5HZ = 0,
    MLX90640_RATE_1HZ   = 1,
    MLX90640_RATE_2HZ   = 2,
    MLX90640_RATE_4HZ   = 3,
    MLX90640_RATE_8HZ   = 4,
    MLX90640_RATE_16HZ  = 5,
    MLX90640_RATE_32HZ  = 6,
    MLX90640_RATE_64HZ  = 7,
} Mlx90640Rate;

/** Calibration parameters stored in EEPROM */
typedef struct {
    int16_t kVdd;
    int16_t vdd25;
    float KvPTAT;
    float KtPTAT;
    uint16_t vPTAT25;
    float alphaPTAT;
    int16_t gainEE;
    float tgc;
    float cpKv;
    float cpKta;
    uint8_t resolutionEE;
    uint8_t calibrationModeEE;
    float KsTa;
    float ksTo[5];
    int16_t ct[5];
    uint16_t alpha[MLX90640_PIXELS];
    uint8_t alphaScale;
    int16_t offset[MLX90640_PIXELS];
    int8_t kta[MLX90640_PIXELS];
    uint8_t ktaScale;
    int8_t kv[MLX90640_PIXELS];
    uint8_t kvScale;
    float cpAlpha[2];
    int16_t cpOffset[2];
    float ilChessC[3];
    uint16_t brokenPixels[5];
    uint16_t outlierPixels[5];
} Mlx90640Params;

/** Device handle */
struct Device;

/**
 * @brief Initialize MLX90640 and extract calibration data.
 * @param i2c_device Pointer to the parent I2C controller Device
 * @param params Output: calibration parameters
 * @return true on success
 */
bool mlx90640_init(struct Device* i2c_device, Mlx90640Params* params);

/**
 * @brief Set the refresh rate of the sensor.
 * @param i2c_device Parent I2C controller Device
 * @param rate Desired refresh rate
 * @return true on success
 */
bool mlx90640_set_refresh_rate(struct Device* i2c_device, Mlx90640Rate rate);

/**
 * @brief Read one full frame from the sensor.
 * @param i2c_device Parent I2C controller Device
 * @param params Calibration parameters from mlx90640_init()
 * @param frame_data Output array of MLX90640_PIXELS floats (°C)
 * @return true on success
 */
bool mlx90640_read_frame(
    struct Device* i2c_device,
    const Mlx90640Params* params,
    float frame_data[MLX90640_PIXELS]
);

#ifdef __cplusplus
}
#endif
