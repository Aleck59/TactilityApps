#include "mlx90640.h"

#include <string.h>
#include <stdio.h>

#include "TactilityHeadless.h"
#include "Kernel/Log.h"
#include "Kernel/I2cController.h"
#include "lvgl.h"

#define TAG "ThermalCamera"

/* -------------------------------------------------------------------------
 * Color palette (Iron / Rainbow)
 * ---------------------------------------------------------------------- */

#define PALETTE_SIZE 256

static uint16_t s_palette[PALETTE_SIZE]; // RGB565

static void build_iron_palette(void) {
    // Iron palette: black → blue → purple → red → orange → yellow → white
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
    const int num_stops = sizeof(stops) / sizeof(stops[0]);

    for (int i = 0; i < PALETTE_SIZE; i++) {
        float t = (float)i / (float)(PALETTE_SIZE - 1);
        int seg = 0;
        for (int s = 0; s < num_stops - 1; s++) {
            if (t >= stops[s].pos && t <= stops[s + 1].pos) { seg = s; break; }
        }
        float local = (t - stops[seg].pos) / (stops[seg + 1].pos - stops[seg].pos);
        uint8_t r = (uint8_t)(stops[seg].r + local * (stops[seg + 1].r - stops[seg].r));
        uint8_t g = (uint8_t)(stops[seg].g + local * (stops[seg + 1].g - stops[seg].g));
        uint8_t b = (uint8_t)(stops[seg].b + local * (stops[seg + 1].b - stops[seg].b));
        // Pack RGB565
        s_palette[i] = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3);
    }
}

/* -------------------------------------------------------------------------
 * App state
 * ---------------------------------------------------------------------- */

typedef struct {
    // Sensor
    Mlx90640Params params;
    bool           sensor_ok;
    float          frame[MLX90640_PIXELS];
    float          t_min;
    float          t_max;
    float          t_center;

    // UI
    lv_obj_t*  canvas;
    lv_obj_t*  label_min;
    lv_obj_t*  label_max;
    lv_obj_t*  label_center;
    lv_obj_t*  label_status;
    lv_timer_t* timer;

    // Canvas pixel buffer (32×24 upscaled to display)
    uint8_t  canvas_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(MLX90640_WIDTH * 6, MLX90640_HEIGHT * 6)];

    // I2C parent device (resolved at start)
    struct Device* i2c;
} AppState;

static AppState* s_state = NULL;

/* -------------------------------------------------------------------------
 * Rendering
 * ---------------------------------------------------------------------- */

#define SCALE 6  // Each MLX pixel → 6×6 display pixels

static void render_frame(AppState* state) {
    if (!state->canvas) return;

    float t_min = state->frame[0];
    float t_max = state->frame[0];
    for (int i = 1; i < MLX90640_PIXELS; i++) {
        if (state->frame[i] < t_min) t_min = state->frame[i];
        if (state->frame[i] > t_max) t_max = state->frame[i];
    }
    float range = t_max - t_min;
    if (range < 0.1f) range = 0.1f;

    state->t_min    = t_min;
    state->t_max    = t_max;
    state->t_center = state->frame[(MLX90640_HEIGHT / 2) * MLX90640_WIDTH + MLX90640_WIDTH / 2];

    lv_draw_buf_t* draw_buf = lv_canvas_get_draw_buf(state->canvas);

    for (int row = 0; row < MLX90640_HEIGHT; row++) {
        for (int col = 0; col < MLX90640_WIDTH; col++) {
            float t   = state->frame[row * MLX90640_WIDTH + col];
            int   idx = (int)((t - t_min) / range * (PALETTE_SIZE - 1));
            if (idx < 0) idx = 0;
            if (idx >= PALETTE_SIZE) idx = PALETTE_SIZE - 1;
            uint16_t color565 = s_palette[idx];
            lv_color_t color = lv_color_make(
                (color565 >> 11) << 3,
                ((color565 >> 5) & 0x3F) << 2,
                (color565 & 0x1F) << 3
            );

            // Draw SCALE×SCALE block
            for (int dy = 0; dy < SCALE; dy++) {
                for (int dx = 0; dx < SCALE; dx++) {
                    lv_canvas_set_px(
                        state->canvas,
                        col * SCALE + dx,
                        row * SCALE + dy,
                        color, LV_OPA_COVER
                    );
                }
            }
        }
    }
    // Draw center crosshair
    int cx = (MLX90640_WIDTH / 2) * SCALE + SCALE / 2;
    int cy = (MLX90640_HEIGHT / 2) * SCALE + SCALE / 2;
    lv_color_t white = lv_color_white();
    for (int d = -6; d <= 6; d++) {
        lv_canvas_set_px(state->canvas, cx + d, cy,     white, LV_OPA_COVER);
        lv_canvas_set_px(state->canvas, cx,     cy + d, white, LV_OPA_COVER);
    }

    lv_obj_invalidate(state->canvas);
}

/* -------------------------------------------------------------------------
 * Timer callback – read sensor & update UI
 * ---------------------------------------------------------------------- */

static void on_timer(lv_timer_t* timer) {
    AppState* state = (AppState*)lv_timer_get_user_data(timer);

    if (!state->sensor_ok || !state->i2c) {
        lv_label_set_text(state->label_status, "No sensor");
        return;
    }

    if (!mlx90640_read_frame(state->i2c, &state->params, state->frame)) {
        lv_label_set_text(state->label_status, "Read error");
        return;
    }

    render_frame(state);

    char buf[32];
    snprintf(buf, sizeof(buf), "Min: %.1f °C", (double)state->t_min);
    lv_label_set_text(state->label_min, buf);
    snprintf(buf, sizeof(buf), "Max: %.1f °C", (double)state->t_max);
    lv_label_set_text(state->label_max, buf);
    snprintf(buf, sizeof(buf), "Ctr: %.1f °C", (double)state->t_center);
    lv_label_set_text(state->label_center, buf);
    lv_label_set_text(state->label_status, "");
}

/* -------------------------------------------------------------------------
 * Lifecycle callbacks
 * ---------------------------------------------------------------------- */

static void on_show(AppContext* app, lv_obj_t* parent) {
    if (!s_state) {
        TT_LOG_E(TAG, "State is NULL in onShow");
        return;
    }
    AppState* state = s_state;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Toolbar
    lv_obj_t* toolbar = tt_toolbar_create(parent, app);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    // Canvas
    int canvas_w = MLX90640_WIDTH * SCALE;
    int canvas_h = MLX90640_HEIGHT * SCALE;
    state->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(state->canvas, state->canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB888);
    lv_canvas_fill_bg(state->canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_align(state->canvas, LV_ALIGN_TOP_MID, 0, 0);

    // Info labels row
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    state->label_min = lv_label_create(row);
    lv_label_set_text(state->label_min, "Min: --.- °C");
    lv_obj_set_style_text_font(state->label_min, &lv_font_montserrat_12, 0);

    state->label_center = lv_label_create(row);
    lv_label_set_text(state->label_center, "Ctr: --.- °C");
    lv_obj_set_style_text_font(state->label_center, &lv_font_montserrat_12, 0);

    state->label_max = lv_label_create(row);
    lv_label_set_text(state->label_max, "Max: --.- °C");
    lv_obj_set_style_text_font(state->label_max, &lv_font_montserrat_12, 0);

    state->label_status = lv_label_create(parent);
    lv_label_set_text(state->label_status, state->sensor_ok ? "" : "Sensor not found");
    lv_obj_set_style_text_color(state->label_status, lv_color_make(255, 80, 80), 0);

    // Timer: update every 500 ms (2 Hz)
    state->timer = lv_timer_create(on_timer, 500, state);
}

static void on_hide(AppContext* app) {
    (void)app;
    if (!s_state) return;
    if (s_state->timer) {
        lv_timer_delete(s_state->timer);
        s_state->timer = NULL;
    }
    s_state->canvas       = NULL;
    s_state->label_min    = NULL;
    s_state->label_max    = NULL;
    s_state->label_center = NULL;
    s_state->label_status = NULL;
}

static void on_start(AppContext* app) {
    (void)app;

    s_state = (AppState*)malloc(sizeof(AppState));
    if (!s_state) { TT_LOG_E(TAG, "OOM"); return; }
    memset(s_state, 0, sizeof(AppState));

    build_iron_palette();

    // Resolve I2C controller from app context (device tree)
    // The device must be registered with name "i2c0" in the board configuration.
    s_state->i2c = tt_find_device_by_name("i2c0");
    if (!s_state->i2c) {
        TT_LOG_W(TAG, "i2c0 not found, trying i2c1");
        s_state->i2c = tt_find_device_by_name("i2c1");
    }

    if (!s_state->i2c) {
        TT_LOG_E(TAG, "No I2C controller found");
        s_state->sensor_ok = false;
        return;
    }

    if (!mlx90640_init(s_state->i2c, &s_state->params)) {
        TT_LOG_E(TAG, "MLX90640 init failed");
        s_state->sensor_ok = false;
        return;
    }

    mlx90640_set_refresh_rate(s_state->i2c, MLX90640_RATE_2HZ);
    s_state->sensor_ok = true;
    TT_LOG_I(TAG, "MLX90640 ready");
}

static void on_stop(AppContext* app) {
    (void)app;
    if (s_state) {
        free(s_state);
        s_state = NULL;
    }
}

/* -------------------------------------------------------------------------
 * App descriptor
 * ---------------------------------------------------------------------- */

const AppManifest thermal_camera_app = {
    .id       = "thermal_camera",
    .name     = "Thermal Camera",
    .icon     = NULL,
    .type     = AppTypeTool,
    .onStart  = on_start,
    .onStop   = on_stop,
    .onShow   = on_show,
    .onHide   = on_hide,
};
