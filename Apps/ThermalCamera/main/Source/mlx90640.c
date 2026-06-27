#include "mlx90640.h"

#include <math.h>
#include <string.h>

#include "Kernel/Log.h"
#include "Kernel/I2cController.h"

#define TAG "MLX90640"

/* -------------------------------------------------------------------------
 * Low-level I2C helpers
 * ---------------------------------------------------------------------- */

static bool mlx_write_reg(struct Device* i2c, uint16_t reg, uint16_t value) {
    uint8_t buf[4] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF)
    };
    return i2c_controller_write(i2c, MLX90640_ADDR, buf, 4, pdMS_TO_TICKS(50)) == ERROR_NONE;
}

static bool mlx_read_regs(struct Device* i2c, uint16_t start_addr, uint16_t* out, uint16_t count) {
    uint8_t addr_buf[2] = { (uint8_t)(start_addr >> 8), (uint8_t)(start_addr & 0xFF) };
    if (i2c_controller_write(i2c, MLX90640_ADDR, addr_buf, 2, pdMS_TO_TICKS(50)) != ERROR_NONE) {
        TT_LOG_E(TAG, "Write addr failed");
        return false;
    }
    // Read back as big-endian uint16 words
    uint8_t* raw = (uint8_t*)out;
    if (i2c_controller_read(i2c, MLX90640_ADDR, raw, count * 2, pdMS_TO_TICKS(200)) != ERROR_NONE) {
        TT_LOG_E(TAG, "Read regs failed");
        return false;
    }
    // Swap bytes (big-endian → host)
    for (uint16_t i = 0; i < count; i++) {
        uint8_t hi = raw[i * 2];
        uint8_t lo = raw[i * 2 + 1];
        out[i] = ((uint16_t)hi << 8) | lo;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * EEPROM / Calibration extraction (based on the MLX90640 datasheet rev 3)
 * ---------------------------------------------------------------------- */

#define EEPROM_SIZE 832

static bool mlx_read_eeprom(struct Device* i2c, uint16_t eeprom[EEPROM_SIZE]) {
    return mlx_read_regs(i2c, 0x2400, eeprom, EEPROM_SIZE);
}

static void mlx_extract_vdd(const uint16_t* ee, Mlx90640Params* p) {
    p->kVdd = (int16_t)((ee[51] & 0xFF00) >> 8);
    if (p->kVdd > 127) p->kVdd -= 256;
    p->kVdd *= 32;
    p->vdd25 = ee[51] & 0x00FF;
    p->vdd25 = ((p->vdd25 - 256) << 5) - 8192;
}

static void mlx_extract_ptat(const uint16_t* ee, Mlx90640Params* p) {
    p->KvPTAT = (float)((ee[50] & 0xFC00) >> 10) / 4096.0f;
    p->KtPTAT = (float)(ee[50] & 0x03FF);
    if (p->KtPTAT > 511.0f) p->KtPTAT -= 1024.0f;
    p->KtPTAT /= 8.0f;
    p->vPTAT25 = ee[49];
    p->alphaPTAT = (float)((ee[16] & 0xF000) >> 14) / 4.0f + 8.0f;
}

static void mlx_extract_gain(const uint16_t* ee, Mlx90640Params* p) {
    p->gainEE = (int16_t)ee[48];
    if (p->gainEE > 32767) p->gainEE -= 65536;
}

static void mlx_extract_tgc(const uint16_t* ee, Mlx90640Params* p) {
    p->tgc = (float)(int8_t)(ee[60] & 0x00FF) / 32.0f;
}

static void mlx_extract_res_corr(const uint16_t* ee, Mlx90640Params* p) {
    p->resolutionEE = (ee[56] & 0x3000) >> 12;
}

static void mlx_extract_ks_ta(const uint16_t* ee, Mlx90640Params* p) {
    int8_t ks = (int8_t)((ee[60] & 0xFF00) >> 8);
    p->KsTa = (float)ks / 8192.0f;
}

static void mlx_extract_ks_to(const uint16_t* ee, Mlx90640Params* p) {
    int8_t step = (int8_t)((ee[63] & 0x3000) >> 12) * 10;
    p->ct[0] = -40;
    p->ct[1] = 0;
    p->ct[2] = (int16_t)((ee[63] & 0x00F0) >> 4) * step;
    p->ct[3] = (int16_t)((ee[63] & 0x0F00) >> 8) * step + p->ct[2];
    p->ct[4] = 400;

    uint8_t ksToScale = (ee[63] & 0x000F) + 8;
    p->ksTo[0] = (float)(int8_t)(ee[61] & 0x00FF) / (float)(1 << ksToScale);
    p->ksTo[1] = (float)(int8_t)((ee[61] & 0xFF00) >> 8) / (float)(1 << ksToScale);
    p->ksTo[2] = (float)(int8_t)(ee[62] & 0x00FF) / (float)(1 << ksToScale);
    p->ksTo[3] = (float)(int8_t)((ee[62] & 0xFF00) >> 8) / (float)(1 << ksToScale);
    p->ksTo[4] = -0.0002f;
}

static void mlx_extract_cp_params(const uint16_t* ee, Mlx90640Params* p) {
    float alphaScale = (float)(1 << ((ee[32] & 0xF000) >> 12));

    int16_t cpOffset0 = (int16_t)(ee[58] & 0x03FF);
    if (cpOffset0 > 511) cpOffset0 -= 1024;
    p->cpOffset[0] = cpOffset0;

    int16_t cpOffset1 = (int16_t)((ee[58] & 0xFC00) >> 10);
    if (cpOffset1 > 31) cpOffset1 -= 64;
    p->cpOffset[1] = cpOffset1 + cpOffset0;

    float cpAlpha0 = (float)(ee[57] & 0x03FF);
    if (cpAlpha0 > 511.0f) cpAlpha0 -= 1024.0f;
    p->cpAlpha[0] = cpAlpha0 / alphaScale;

    float cpAlpha1 = (float)((ee[57] & 0xFC00) >> 10);
    if (cpAlpha1 > 31.0f) cpAlpha1 -= 64.0f;
    p->cpAlpha[1] = p->cpAlpha[0] * (1.0f + cpAlpha1 / 128.0f);

    p->cpKta = (float)(int8_t)(ee[59] & 0x00FF) / 8192.0f;
    p->cpKv  = (float)(int8_t)((ee[59] & 0xFF00) >> 8) / 16.0f;
    p->tgc   = (float)(int8_t)(ee[60] & 0x00FF) / 32.0f;
}

static void mlx_extract_alpha_pixels(const uint16_t* ee, Mlx90640Params* p) {
    uint8_t aScale  = ((ee[32] & 0xF000) >> 12) + 30;
    int16_t aRef    = (int16_t)ee[33];
    float   aAlpha  = (float)((ee[32] & 0x0F00) >> 8);

    for (int i = 1; i <= 4; i++) {
        for (int j = 1; j <= 4; j++) {
            int p_idx = (i - 1) * 4 + (j - 1);
            float alphaTemp = (float)(ee[64 + p_idx] & 0x03F0) >> 4;
            if (alphaTemp > 31.0f) alphaTemp -= 64.0f;
            p->alpha[p_idx] = (uint16_t)alphaTemp;
        }
    }
    p->alphaScale = aScale;
    (void)aRef; (void)aAlpha;

    // Full pixel alpha table
    uint8_t accRow[24], accCol[32];
    for (int i = 0; i < 6; i++) {
        int p_idx = i * 4;
        int16_t tmp = (int16_t)((ee[65 + i] & 0xF000) >> 12);
        if (tmp > 7) tmp -= 16;
        accRow[p_idx]     = (uint8_t)tmp;
        tmp = (int16_t)((ee[65 + i] & 0x0F00) >> 8);
        if (tmp > 7) tmp -= 16;
        accRow[p_idx + 1] = (uint8_t)tmp;
        tmp = (int16_t)((ee[65 + i] & 0x00F0) >> 4);
        if (tmp > 7) tmp -= 16;
        accRow[p_idx + 2] = (uint8_t)tmp;
        tmp = (int16_t)(ee[65 + i] & 0x000F);
        if (tmp > 7) tmp -= 16;
        accRow[p_idx + 3] = (uint8_t)tmp;
    }
    for (int i = 0; i < 8; i++) {
        int p_idx = i * 4;
        int16_t tmp = (int16_t)((ee[71 + i] & 0xF000) >> 12);
        if (tmp > 7) tmp -= 16;
        accCol[p_idx]     = (uint8_t)tmp;
        tmp = (int16_t)((ee[71 + i] & 0x0F00) >> 8);
        if (tmp > 7) tmp -= 16;
        accCol[p_idx + 1] = (uint8_t)tmp;
        tmp = (int16_t)((ee[71 + i] & 0x00F0) >> 4);
        if (tmp > 7) tmp -= 16;
        accCol[p_idx + 2] = (uint8_t)tmp;
        tmp = (int16_t)(ee[71 + i] & 0x000F);
        if (tmp > 7) tmp -= 16;
        accCol[p_idx + 3] = (uint8_t)tmp;
    }

    float alphRef = (float)ee[33];
    for (int i = 0; i < MLX90640_HEIGHT; i++) {
        for (int j = 0; j < MLX90640_WIDTH; j++) {
            int idx = i * MLX90640_WIDTH + j;
            int16_t alphaTemp = (int16_t)((ee[64 + idx] & 0x03F0) >> 4);
            if (alphaTemp > 31) alphaTemp -= 64;
            float alpha_f = (alphRef + accRow[i] * aAlpha + accCol[j] * aAlpha + alphaTemp) /
                            (float)(1UL << aScale);
            p->alpha[idx] = (uint16_t)(alpha_f * 65535.0f);
        }
    }
}

static void mlx_extract_offset_pixels(const uint16_t* ee, Mlx90640Params* p) {
    int16_t offsetRef = (int16_t)ee[49];
    if (offsetRef > 32767) offsetRef -= 65536;

    for (int i = 0; i < MLX90640_HEIGHT; i++) {
        for (int j = 0; j < MLX90640_WIDTH; j++) {
            int idx = i * MLX90640_WIDTH + j;
            int16_t offsetTemp = (int16_t)((ee[64 + idx] & 0xFC00) >> 10);
            if (offsetTemp > 31) offsetTemp -= 64;
            p->offset[idx] = offsetRef + offsetTemp;
        }
    }
}

static void mlx_extract_kta_pixels(const uint16_t* ee, Mlx90640Params* p) {
    int8_t ktaRCp[4] = {
        (int8_t)((ee[54] & 0xFF00) >> 8),
        (int8_t)(ee[54] & 0x00FF),
        (int8_t)((ee[55] & 0xFF00) >> 8),
        (int8_t)(ee[55] & 0x00FF),
    };
    uint8_t ktaScale1 = ((ee[56] & 0x00F0) >> 4) + 8;

    for (int i = 0; i < MLX90640_HEIGHT; i++) {
        for (int j = 0; j < MLX90640_WIDTH; j++) {
            int idx = i * MLX90640_WIDTH + j;
            int8_t split = (i & 1) * 2 + (j & 1);
            int8_t ktaTemp = (int8_t)((ee[64 + idx] & 0x000E) >> 1);
            if (ktaTemp > 3) ktaTemp -= 8;
            p->kta[idx] = (int8_t)((ktaRCp[split] + ktaTemp) / (float)(1 << ktaScale1) * 127.0f);
        }
    }
    p->ktaScale = ktaScale1;
}

static void mlx_extract_kv_pixels(const uint16_t* ee, Mlx90640Params* p) {
    int8_t kvRCp[4] = {
        (int8_t)((ee[52] & 0xF000) >> 12),
        (int8_t)((ee[52] & 0x0F00) >> 8),
        (int8_t)((ee[52] & 0x00F0) >> 4),
        (int8_t)(ee[52] & 0x000F),
    };
    for (int k = 0; k < 4; k++) if (kvRCp[k] > 7) kvRCp[k] -= 16;

    uint8_t kvScale = (ee[56] & 0x0F00) >> 8;

    for (int i = 0; i < MLX90640_HEIGHT; i++) {
        for (int j = 0; j < MLX90640_WIDTH; j++) {
            int idx = i * MLX90640_WIDTH + j;
            int8_t split = (i & 1) * 2 + (j & 1);
            p->kv[idx] = (int8_t)(kvRCp[split] / (float)(1 << kvScale) * 127.0f);
        }
    }
    p->kvScale = kvScale;
}

static void mlx_extract_il_chess_c(const uint16_t* ee, Mlx90640Params* p) {
    p->calibrationModeEE = (ee[10] & 0x0800) >> 4;
    float ilChessScale = (float)(1 << 5);
    int16_t tmp;

    tmp = (int16_t)(ee[53] & 0x003F);
    if (tmp > 31) tmp -= 64;
    p->ilChessC[0] = (float)tmp / ilChessScale;

    tmp = (int16_t)((ee[53] & 0x07C0) >> 6);
    if (tmp > 15) tmp -= 32;
    p->ilChessC[1] = (float)tmp / ilChessScale;

    tmp = (int16_t)((ee[53] & 0xF800) >> 11);
    if (tmp > 15) tmp -= 32;
    p->ilChessC[2] = (float)tmp / ilChessScale;
}

/* -------------------------------------------------------------------------
 * Temperature calculation
 * ---------------------------------------------------------------------- */

static float mlx_get_vdd(const uint16_t* frame, const Mlx90640Params* p) {
    int16_t vdd_raw = (int16_t)frame[810];
    if (vdd_raw > 32767) vdd_raw -= 65536;
    int res_corr = (1 << p->resolutionEE) / (1 << ((frame[832] & 0x0C00) >> 10));
    return ((float)(res_corr * vdd_raw - p->vdd25)) / (float)p->kVdd + 3.3f;
}

static float mlx_get_ta(const uint16_t* frame, const Mlx90640Params* p) {
    float vdd = mlx_get_vdd(frame, p);
    int16_t ptat_raw = (int16_t)frame[800];
    if (ptat_raw > 32767) ptat_raw -= 65536;
    float vptat_art = (float)ptat_raw / ((float)ptat_raw * p->alphaPTAT + (float)frame[768]) * 262144.0f;
    return (vptat_art / (1.0f + p->KvPTAT * (vdd - 3.3f)) - (float)p->vPTAT25) /
           p->KtPTAT + 25.0f;
}

static float mlx_get_gain(const uint16_t* frame, const Mlx90640Params* p) {
    int16_t raw = (int16_t)frame[778];
    if (raw > 32767) raw -= 65536;
    return (float)p->gainEE / (float)raw;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

bool mlx90640_init(struct Device* i2c, Mlx90640Params* p) {
    uint16_t eeprom[EEPROM_SIZE];
    TT_LOG_I(TAG, "Reading EEPROM...");
    if (!mlx_read_eeprom(i2c, eeprom)) {
        TT_LOG_E(TAG, "EEPROM read failed");
        return false;
    }
    mlx_extract_vdd(eeprom, p);
    mlx_extract_ptat(eeprom, p);
    mlx_extract_gain(eeprom, p);
    mlx_extract_tgc(eeprom, p);
    mlx_extract_res_corr(eeprom, p);
    mlx_extract_ks_ta(eeprom, p);
    mlx_extract_ks_to(eeprom, p);
    mlx_extract_cp_params(eeprom, p);
    mlx_extract_alpha_pixels(eeprom, p);
    mlx_extract_offset_pixels(eeprom, p);
    mlx_extract_kta_pixels(eeprom, p);
    mlx_extract_kv_pixels(eeprom, p);
    mlx_extract_il_chess_c(eeprom, p);
    TT_LOG_I(TAG, "Init OK");
    return true;
}

bool mlx90640_set_refresh_rate(struct Device* i2c, Mlx90640Rate rate) {
    uint16_t ctrl;
    if (!mlx_read_regs(i2c, 0x800D, &ctrl, 1)) return false;
    ctrl = (ctrl & 0xFC7F) | ((uint16_t)(rate & 0x07) << 7);
    return mlx_write_reg(i2c, 0x800D, ctrl);
}

bool mlx90640_read_frame(
    struct Device* i2c,
    const Mlx90640Params* p,
    float frame_data[MLX90640_PIXELS])
{
    // Read two sub-frames and combine
    uint16_t frame[834];
    float ta, gain, vdd;

    for (int subpage = 0; subpage < 2; subpage++) {
        // Wait for data ready
        uint16_t status;
        int retries = 10;
        do {
            if (!mlx_read_regs(i2c, 0x8000, &status, 1)) return false;
            if (status & 0x0008) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        } while (--retries > 0);

        if (retries == 0) { TT_LOG_W(TAG, "Frame timeout"); return false; }

        // Clear new data flag
        mlx_write_reg(i2c, 0x8000, status & ~0x0008);

        // Read RAM (0x0400 .. 0x073F = 832 words)
        if (!mlx_read_regs(i2c, 0x0400, frame, 832)) return false;
        // Read control register
        if (!mlx_read_regs(i2c, 0x800D, &frame[832], 1)) return false;

        ta   = mlx_get_ta(frame, p);
        gain = mlx_get_gain(frame, p);
        vdd  = mlx_get_vdd(frame, p);

        // CP compensation
        float cpAlpha0 = p->cpAlpha[0];
        float cpAlpha1 = p->cpAlpha[1];
        float cpGain0  = p->cpOffset[0] * gain * (1.0f + p->cpKta * (ta - 25.0f)) *
                         (1.0f + p->cpKv * (vdd - 3.3f));
        float cpGain1  = p->cpOffset[1] * gain * (1.0f + p->cpKta * (ta - 25.0f)) *
                         (1.0f + p->cpKv * (vdd - 3.3f));

        for (int i = 0; i < MLX90640_HEIGHT; i++) {
            for (int j = 0; j < MLX90640_WIDTH; j++) {
                int idx = i * MLX90640_WIDTH + j;
                if ((idx % 2) != subpage) continue;

                // Raw IR value
                int16_t ir_raw = (int16_t)frame[idx];
                if (ir_raw > 32767) ir_raw -= 65536;

                float kta_f = (float)p->kta[idx] / (float)(1 << p->ktaScale);
                float kv_f  = (float)p->kv[idx]  / (float)(1 << p->kvScale);

                float irData = ((float)ir_raw -
                                (float)p->offset[idx] * gain *
                                (1.0f + kta_f * (ta - 25.0f)) *
                                (1.0f + kv_f  * (vdd - 3.3f)));

                // IL chess compensation
                if (p->calibrationModeEE == 0) {
                    float il = p->ilChessC[2] * (2.0f * (float)(i & 1) - 1.0f) -
                               p->ilChessC[1] * (2.0f * (float)(j & 1) - 1.0f) -
                               p->ilChessC[0];
                    irData -= il;
                }

                // TGC compensation
                float alpha_f = (float)p->alpha[idx] / 65535.0f;
                float cpGain  = (idx % 2 == 0) ? cpGain0 : cpGain1;
                float cpAlpha = (idx % 2 == 0) ? cpAlpha0 : cpAlpha1;
                irData -= p->tgc * cpGain / cpAlpha * alpha_f;

                // KsTa compensation
                irData /= (1.0f + p->KsTa * (ta - 25.0f));

                // Object temperature
                float tTaK4 = (ta + 273.15f);
                tTaK4 = tTaK4 * tTaK4 * tTaK4 * tTaK4;

                float sx = alpha_f * alpha_f * alpha_f * (irData + alpha_f * tTaK4);
                sx = sqrtf(sqrtf(sx)) * p->ksTo[1];

                float toK4 = irData / (alpha_f * (1.0f - p->ksTo[1] * 273.15f) + sx) + tTaK4;
                frame_data[idx] = sqrtf(sqrtf(toK4)) - 273.15f;
            }
        }
    }
    return true;
}
