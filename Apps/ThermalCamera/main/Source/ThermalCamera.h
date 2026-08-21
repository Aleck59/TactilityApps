/**
 * @file ThermalCamera.h
 * @brief Thermal imaging application built around the MLX90640 sensor.
 *
 * The sensor is polled by a dedicated task so that a slow I2C transfer can
 * never stall the user interface. The task publishes finished frames through a
 * mutex protected buffer; an LVGL timer picks them up, renders the false-colour
 * image and refreshes the readouts.
 */
#pragma once

#include "Mlx90640.h"
#include "Palette.h"
#include "Settings.h"
#include "ThermalImage.h"

#include <TactilityCpp/App.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <lvgl.h>
#include <tactility/device.h>
#include <tt_app.h>

/** MlxBus implementation on top of a Tactility I2C controller device. */
class TactilityI2cBus final : public MlxBus {
public:
    void setTarget(Device* device, uint8_t address) {
        device_ = device;
        address_ = address;
    }

    bool readWords(uint16_t startAddress, uint16_t* out, size_t count) override;
    bool writeWord(uint16_t address, uint16_t value) override;

private:
    Device* device_ = nullptr;
    uint8_t address_ = MLX_DEFAULT_ADDRESS;
};

/** Values the acquisition task publishes for the user interface. */
struct SensorStatus {
    float ambientTemperature = 25.0f;
    float supplyVoltage = 0.0f;
    float framesPerSecond = 0.0f;
    uint32_t frameCounter = 0;
    uint32_t errorCount = 0;
    bool present = false;
    bool ready = false;
    /** Why the sensor is not ready, when it is not. */
    MlxInitStatus initStatus = MLX_INIT_NOT_STARTED;
};

class ThermalCamera final : public App {
public:
    void onCreate(AppHandle app) override;
    void onDestroy(AppHandle app) override;
    void onShow(AppHandle app, lv_obj_t* parent) override;
    void onHide(AppHandle app) override;

private:
    // ---- Sensor plumbing --------------------------------------------------
    bool findSensor();
    void startAcquisition();
    void stopAcquisition();
    static void acquisitionTrampoline(void* context);
    void acquisitionLoop();
    void publishStatus(bool present, bool ready);
    /** Sleep in short slices so shutting the task down stays responsive. */
    void interruptibleDelay(uint32_t milliseconds);
    /** Copy the current settings to the acquisition task and ask it to reconfigure. */
    void publishSettings(bool reconfigure);

    // ---- Buffers ----------------------------------------------------------
    bool allocateBuffers();
    void freeBuffers();
    void rebuildPalette();
    void rebuildColumnMap();

    // ---- View management --------------------------------------------------
    void clearContent();
    void buildCameraView();
    void buildSettingsView();
    void refreshToolbar();

    // ---- Camera view ------------------------------------------------------
    void onUiTick();
    bool fetchFrame();
    void renderImage();
    void updateReadouts();
    void updateControlLabels();
    void updateColorBar();
    void handleImageTouch(int32_t x, int32_t y);
    void saveSnapshot();
    void setStatusMessage(const char* message);

    // ---- Settings view helpers --------------------------------------------
    void applyAndStoreSettings(bool reconfigure);

    // ---- LVGL callbacks ---------------------------------------------------
    static void onUiTimer(lv_timer_t* timer);
    static void onSettingsButton(lv_event_t* event);
    static void onBackButton(lv_event_t* event);
    static void onSaveButton(lv_event_t* event);
    static void onFreezeButton(lv_event_t* event);
    static void onPaletteButton(lv_event_t* event);
    static void onRangeButton(lv_event_t* event);
    static void onMeasureButton(lv_event_t* event);
    static void onImagePressed(lv_event_t* event);
    static void onSettingChanged(lv_event_t* event);
    static void onResetSettings(lv_event_t* event);

    /** Identifies which setting an LVGL control edits. */
    enum SettingKey : int {
        SETTING_PALETTE = 0,
        SETTING_INTERPOLATION,
        SETTING_UNIT,
        SETTING_REFRESH_RATE,
        SETTING_PATTERN,
        SETTING_RESOLUTION,
        SETTING_EMISSIVITY,
        SETTING_REFLECTED,
        SETTING_OFFSET,
        SETTING_NOISE,
        SETTING_RANGE_MINIMUM,
        SETTING_RANGE_MAXIMUM,
        SETTING_ALARM_MODE,
        SETTING_ALARM_LOW,
        SETTING_ALARM_HIGH,
        SETTING_MIRROR,
        SETTING_FLIP,
        SETTING_COLOR_BAR,
        SETTING_CROSSHAIR,
        SETTING_AUTO_RANGE
    };

    /** Attached to every settings control so one callback can serve them all. */
    struct SettingBinding {
        ThermalCamera* camera;
        SettingKey key;
        lv_obj_t* valueLabel;
        /** printf format of the value label, and the factor from slider units. */
        const char* format;
        float displayScale;
    };

    void applySettingFromControl(const SettingBinding& binding, lv_obj_t* control);
    lv_obj_t* addDropdownSetting(
        lv_obj_t* parent,
        const char* title,
        const char* options,
        int selected,
        SettingKey key
    );
    lv_obj_t* addSliderSetting(
        lv_obj_t* parent,
        const char* title,
        int minimum,
        int maximum,
        int value,
        SettingKey key,
        const char* format,
        float displayScale
    );

    /** Rebuilding a view deletes the widget whose callback is running, so both
     *  view switches are deferred to the next LVGL cycle. */
    static void showCameraAsync(void* context);
    static void showSettingsAsync(void* context);
    lv_obj_t* addSwitchSetting(lv_obj_t* parent, const char* title, bool value, SettingKey key);
    void updateSettingValueLabel(const SettingBinding& binding, int rawValue);

    static constexpr int MAX_SETTING_BINDINGS = 24;

    // ---- State ------------------------------------------------------------
    AppHandle appHandle_ = nullptr;
    CameraSettings settings_;

    Device* i2cDevice_ = nullptr;
    TactilityI2cBus bus_;
    /** Held by value: its calibration is about 9 kB, and heap allocating it
     *  would need operator new forms the ELF loader cannot resolve. */
    Mlx90640 sensor_;

    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool taskRunning_ = false;
    volatile bool taskFinished_ = true;
    volatile bool reconfigureRequested_ = true;

    /** Guarded by mutex_: the most recent complete frame and its status. */
    float* sharedFrame_ = nullptr;
    SensorStatus sharedStatus_;
    CameraSettings taskSettings_;

    /** Owned by the acquisition task. */
    float* workFrame_ = nullptr;
    float* filterFrame_ = nullptr;
    float* scratchFrame_ = nullptr;
    uint16_t* rawWords_ = nullptr;

    /** Owned by the user interface. */
    float* displayFrame_ = nullptr;
    uint16_t* imageBuffer_ = nullptr;
    uint16_t* colorBarBuffer_ = nullptr;
    ColumnMap* columnMap_ = nullptr;
    uint16_t palette_[PALETTE_LEVELS] = {};
    uint32_t paletteRgb888_[PALETTE_LEVELS] = {};

    int imageWidth_ = 0;
    int imageHeight_ = 0;
    int colorBarWidth_ = 0;
    int colorBarHeight_ = 0;

    uint32_t lastFrameCounter_ = 0;
    bool haveFrame_ = false;
    bool frozen_ = false;
    bool showingSettings_ = false;
    bool paletteDirty_ = true;
    bool columnMapDirty_ = true;

    FrameStats stats_;
    RegionStats regionStats_;
    SensorStatus status_;
    /** Identity of the last message shown, so idle states do not repaint. */
    const char* lastStatusMessage_ = nullptr;
    float displayRangeMinimum_ = 20.0f;
    float displayRangeMaximum_ = 40.0f;

    // ---- Widgets ----------------------------------------------------------
    lv_obj_t* root_ = nullptr;
    lv_obj_t* toolbar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_timer_t* uiTimer_ = nullptr;

    lv_obj_t* imageCanvas_ = nullptr;
    lv_obj_t* colorBarCanvas_ = nullptr;
    lv_obj_t* colorBarHighLabel_ = nullptr;
    lv_obj_t* colorBarLowLabel_ = nullptr;
    lv_obj_t* spotLabel_ = nullptr;
    lv_obj_t* spotCaptionLabel_ = nullptr;
    lv_obj_t* maximumLabel_ = nullptr;
    lv_obj_t* minimumLabel_ = nullptr;
    lv_obj_t* averageLabel_ = nullptr;
    lv_obj_t* regionLabel_ = nullptr;
    lv_obj_t* detailLabel_ = nullptr;
    lv_obj_t* statusLabel_ = nullptr;

    lv_obj_t* paletteButton_ = nullptr;
    lv_obj_t* rangeButton_ = nullptr;
    lv_obj_t* measureButton_ = nullptr;
    lv_obj_t* freezeButton_ = nullptr;

    SettingBinding bindings_[MAX_SETTING_BINDINGS] = {};
    int bindingCount_ = 0;

    char statusText_[80] = {};
};
