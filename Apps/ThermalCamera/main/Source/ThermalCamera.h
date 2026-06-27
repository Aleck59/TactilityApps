#pragma once

#include "mlx90640.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "TactilityHeadless.h"
#include "TactilityCpp/App.h"
#include "Kernel/Log.h"
#include "Kernel/I2cController.h"
#include "lvgl.h"

#define TAG "ThermalCamera"

/* -------------------------------------------------------------------------
 * Color palette (Iron)
 * ---------------------------------------------------------------------- */

#define PALETTE_SIZE 256

static uint16_t s_palette[PALETTE_SIZE];

static void build_iron_palette() {
    static const struct { float pos; uint8_t r, g, b; } stops[] = {
        { 0.00f,   0,   0,   0 },
        { 0.10f,   0,   0, 128 },
        { 0.20f,  64,   0, 200 },
        { 0.40f, 180,   0, 100 },
        { 0.55f, 220,  60,   0 },
        { 0.70f, 255, 130,   0 },
        { 0.85f, 255, 220,   0 },
        { 1.00f, 255, 255, 255 },
    };
    const int num_stops = (int)(sizeof(stops) / sizeof(stops[0]));

    for (int i = 0; i < PALETTE_SIZE; i++) {
        float t = (float)i / (float)(PALETTE_SIZE - 1);
        int seg = 0;
        for (int s = 0; s < num_stops - 1; s++) {
            if (t >= stops[s].pos && t <= stops[s + 1].pos) { seg = s; break; }
        }
        float range_seg = stops[seg + 1].pos - stops[seg].pos;
        float local = (range_seg > 0.0f) ? (t - stops[seg].pos) / range_seg : 0.0f;
        uint8_t r = (uint8_t)(stops[seg].r + local * (stops[seg + 1].r - stops[seg].r));
        uint8_t g = (uint8_t)(stops[seg].g + local * (stops[seg + 1].g - stops[seg].g));
        uint8_t b = (uint8_t)(stops[seg].b + local * (stops[seg + 1].b - stops[seg].b));
        s_palette[i] = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3);
    }
}

/* -------------------------------------------------------------------------
 * ThermalCamera — C++ App subclass
 * ---------------------------------------------------------------------- */

#define SCALE 6  // Each MLX pixel → 6×6 display pixels

class ThermalCamera : public tt::App {
public:

    // ---- App lifecycle --------------------------------------------------

    void onStart() override {
        build_iron_palette();

        _i2c = tt::kernel::findDevice("i2c0");
        if (!_i2c) {
            TT_LOG_W(TAG, "i2c0 not found, trying i2c1");
            _i2c = tt::kernel::findDevice("i2c1");
        }
        if (!_i2c) {
            TT_LOG_E(TAG, "No I2C controller found");
            _sensor_ok = false;
            return;
        }

        if (!mlx90640_init(_i2c, &_params)) {
            TT_LOG_E(TAG, "MLX90640 init failed");
            _sensor_ok = false;
            return;
        }

        mlx90640_set_refresh_rate(_i2c, MLX90640_RATE_2HZ);
        _sensor_ok = true;
        TT_LOG_I(TAG, "MLX90640 ready");
    }

    void onStop() override {
        _i2c = nullptr;
        _sensor_ok = false;
    }

    void onShow(lv_obj_t* parent) override {
        lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(parent, 4, 0);
        lv_obj_set_style_pad_row(parent, 4, 0);

        // Toolbar
        tt_toolbar_create(parent, getContext());

        // Canvas
        constexpr int canvas_w = MLX90640_WIDTH * SCALE;
        constexpr int canvas_h = MLX90640_HEIGHT * SCALE;
        _canvas = lv_canvas_create(parent);
        lv_canvas_set_buffer(_canvas, _canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB888);
        lv_canvas_fill_bg(_canvas, lv_color_black(), LV_OPA_COVER);
        lv_obj_set_size(_canvas, canvas_w, canvas_h);
        lv_obj_center(_canvas);

        // Labels row
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);

        _label_min = lv_label_create(row);
        lv_label_set_text(_label_min, "Min: --.-");
        lv_obj_set_style_text_font(_label_min, &lv_font_montserrat_12, 0);

        _label_center = lv_label_create(row);
        lv_label_set_text(_label_center, "Ctr: --.-");
        lv_obj_set_style_text_font(_label_center, &lv_font_montserrat_12, 0);

        _label_max = lv_label_create(row);
        lv_label_set_text(_label_max, "Max: --.-");
        lv_obj_set_style_text_font(_label_max, &lv_font_montserrat_12, 0);

        _label_status = lv_label_create(parent);
        lv_label_set_text(_label_status, _sensor_ok ? "" : "Sensor not found");
        lv_obj_set_style_text_color(_label_status, lv_color_make(255, 80, 80), 0);

        // Periodic update timer (500 ms = 2 Hz)
        _timer = lv_timer_create(onTimerStatic, 500, this);
    }

    void onHide() override {
        if (_timer) {
            lv_timer_delete(_timer);
            _timer = nullptr;
        }
        _canvas = nullptr;
        _label_min = nullptr;
        _label_max = nullptr;
        _label_center = nullptr;
        _label_status = nullptr;
    }

private:

    // ---- Sensor state ---------------------------------------------------
    Mlx90640Params  _params{};
    bool            _sensor_ok = false;
    float           _frame[MLX90640_PIXELS]{};
    float           _t_min = 0.0f;
    float           _t_max = 0.0f;
    float           _t_center = 0.0f;
    tt::kernel::Device* _i2c = nullptr;

    // ---- UI state -------------------------------------------------------
    lv_obj_t*   _canvas = nullptr;
    lv_obj_t*   _label_min = nullptr;
    lv_obj_t*   _label_max = nullptr;
    lv_obj_t*   _label_center = nullptr;
    lv_obj_t*   _label_status = nullptr;
    lv_timer_t* _timer = nullptr;

    // Canvas backing buffer
    uint8_t _canvas_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(MLX90640_WIDTH * SCALE, MLX90640_HEIGHT * SCALE)]{};

    // ---- Static timer trampoline ----------------------------------------
    static void onTimerStatic(lv_timer_t* timer) {
        auto* self = static_cast<ThermalCamera*>(lv_timer_get_user_data(timer));
        self->onTimer();
    }

    // ---- Per-tick logic -------------------------------------------------
    void onTimer() {
        if (!_sensor_ok || !_i2c) {
            if (_label_status) lv_label_set_text(_label_status, "No sensor");
            return;
        }

        if (!mlx90640_read_frame(_i2c, &_params, _frame)) {
            if (_label_status) lv_label_set_text(_label_status, "Read error");
            return;
        }

        renderFrame();
        updateLabels();
    }

    // ---- Rendering ------------------------------------------------------
    void renderFrame() {
        if (!_canvas) return;

        float t_min = _frame[0];
        float t_max = _frame[0];
        for (int i = 1; i < MLX90640_PIXELS; i++) {
            if (_frame[i] < t_min) t_min = _frame[i];
            if (_frame[i] > t_max) t_max = _frame[i];
        }
        float range = t_max - t_min;
        if (range < 0.1f) range = 0.1f;

        _t_min    = t_min;
        _t_max    = t_max;
        _t_center = _frame[(MLX90640_HEIGHT / 2) * MLX90640_WIDTH + MLX90640_WIDTH / 2];

        for (int row = 0; row < MLX90640_HEIGHT; row++) {
            for (int col = 0; col < MLX90640_WIDTH; col++) {
                float t = _frame[row * MLX90640_WIDTH + col];
                int idx = (int)(((t - t_min) / range) * (PALETTE_SIZE - 1));
                if (idx < 0)            idx = 0;
                if (idx >= PALETTE_SIZE) idx = PALETTE_SIZE - 1;

                uint16_t c565 = s_palette[idx];
                lv_color_t color = lv_color_make(
                    (uint8_t)((c565 >> 11) << 3),
                    (uint8_t)(((c565 >> 5) & 0x3F) << 2),
                    (uint8_t)((c565 & 0x1F) << 3)
                );

                for (int dy = 0; dy < SCALE; dy++) {
                    for (int dx = 0; dx < SCALE; dx++) {
                        lv_canvas_set_px(_canvas,
                            col * SCALE + dx,
                            row * SCALE + dy,
                            color, LV_OPA_COVER);
                    }
                }
            }
        }

        // Crosshair
        int cx = (MLX90640_WIDTH  / 2) * SCALE + SCALE / 2;
        int cy = (MLX90640_HEIGHT / 2) * SCALE + SCALE / 2;
        lv_color_t white = lv_color_white();
        for (int d = -6; d <= 6; d++) {
            lv_canvas_set_px(_canvas, cx + d, cy,     white, LV_OPA_COVER);
            lv_canvas_set_px(_canvas, cx,     cy + d, white, LV_OPA_COVER);
        }

        lv_obj_invalidate(_canvas);
    }

    void updateLabels() {
        if (!_label_min) return;
        char buf[24];
        snprintf(buf, sizeof(buf), "Min:%.1f", (double)_t_min);
        lv_label_set_text(_label_min, buf);
        snprintf(buf, sizeof(buf), "Ctr:%.1f", (double)_t_center);
        lv_label_set_text(_label_center, buf);
        snprintf(buf, sizeof(buf), "Max:%.1f", (double)_t_max);
        lv_label_set_text(_label_max, buf);
        lv_label_set_text(_label_status, "");
    }
};
