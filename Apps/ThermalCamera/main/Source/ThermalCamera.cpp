#include "ThermalCamera.h"

#include "Ui.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <tactility/drivers/i2c_controller.h>
#include <tt_lvgl_toolbar.h>

namespace {

constexpr auto* TAG = "ThermalCamera";

/** Words per I2C transfer. Keeps the bus free for the touch controller. */
constexpr size_t I2C_CHUNK_WORDS = 208;
constexpr TickType_t I2C_TIMEOUT = pdMS_TO_TICKS(200);
constexpr TickType_t I2C_PROBE_TIMEOUT = pdMS_TO_TICKS(30);

/** Smallest and largest magnification of the 32x24 array. */
constexpr int MINIMUM_SCALE = 2;
constexpr int MAXIMUM_SCALE = 16;

struct SensorSearch {
    uint8_t address;
    Device* found;
};

bool onI2cController(Device* device, void* context) {
    auto* search = static_cast<SensorSearch*>(context);
    if (!device_is_ready(device)) return true;
    if (i2c_controller_has_device_at_address(device, search->address, I2C_PROBE_TIMEOUT) == ERROR_NONE) {
        search->found = device;
        return false; // stop iterating
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// I2C transport
// ---------------------------------------------------------------------------

bool TactilityI2cBus::readWords(uint16_t startAddress, uint16_t* out, size_t count) {
    if (device_ == nullptr || out == nullptr) return false;

    size_t offset = 0;
    while (offset < count) {
        const size_t chunk = (count - offset) < I2C_CHUNK_WORDS ? (count - offset) : I2C_CHUNK_WORDS;
        const uint16_t chunkAddress = static_cast<uint16_t>(startAddress + offset);
        const uint8_t addressBytes[2] = {
            static_cast<uint8_t>(chunkAddress >> 8),
            static_cast<uint8_t>(chunkAddress & 0xFF)
        };

        auto* destination = reinterpret_cast<uint8_t*>(out + offset);
        if (i2c_controller_write_read(device_, address_, addressBytes, 2, destination, chunk * 2, I2C_TIMEOUT) !=
            ERROR_NONE) {
            return false;
        }

        // The sensor sends big-endian words; convert them in place.
        for (size_t i = 0; i < chunk; i++) {
            const uint8_t high = destination[i * 2];
            const uint8_t low = destination[i * 2 + 1];
            out[offset + i] = static_cast<uint16_t>((static_cast<uint16_t>(high) << 8) | low);
        }
        offset += chunk;
    }
    return true;
}

bool TactilityI2cBus::writeWord(uint16_t address, uint16_t value) {
    if (device_ == nullptr) return false;
    const uint8_t buffer[4] = {
        static_cast<uint8_t>(address >> 8),
        static_cast<uint8_t>(address & 0xFF),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xFF)
    };
    return i2c_controller_write(device_, address_, buffer, 4, I2C_TIMEOUT) == ERROR_NONE;
}

// ---------------------------------------------------------------------------
// Application lifecycle
// ---------------------------------------------------------------------------

void ThermalCamera::onCreate(AppHandle app) {
    appHandle_ = app;

    settingsLoad(settings_);
    taskSettings_ = settings_;
    displayRangeMinimum_ = settings_.manualRangeMinimum;
    displayRangeMaximum_ = settings_.manualRangeMaximum;

    mutex_ = xSemaphoreCreateMutex();

    sharedFrame_ = static_cast<float*>(heap_caps_calloc(MLX_PIXELS, sizeof(float), MALLOC_CAP_8BIT));
    workFrame_ = static_cast<float*>(heap_caps_calloc(MLX_PIXELS, sizeof(float), MALLOC_CAP_8BIT));
    filterFrame_ = static_cast<float*>(heap_caps_calloc(MLX_PIXELS, sizeof(float), MALLOC_CAP_8BIT));
    scratchFrame_ = static_cast<float*>(heap_caps_calloc(MLX_PIXELS, sizeof(float), MALLOC_CAP_8BIT));
    displayFrame_ = static_cast<float*>(heap_caps_calloc(MLX_PIXELS, sizeof(float), MALLOC_CAP_8BIT));
    // The I2C driver reads into this buffer, so keep it in internal memory.
    rawWords_ = static_cast<uint16_t*>(heap_caps_calloc(MLX_FRAME_WORDS, sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (sharedFrame_ == nullptr || workFrame_ == nullptr || filterFrame_ == nullptr ||
        scratchFrame_ == nullptr || displayFrame_ == nullptr || rawWords_ == nullptr) {
        ESP_LOGE(TAG, "Out of memory during startup");
        return;
    }

    rebuildPalette();
    startAcquisition();
}

void ThermalCamera::onDestroy(AppHandle app) {
    stopAcquisition();
    settingsSave(settings_);

    freeBuffers();

    heap_caps_free(sharedFrame_);
    heap_caps_free(workFrame_);
    heap_caps_free(filterFrame_);
    heap_caps_free(scratchFrame_);
    heap_caps_free(displayFrame_);
    heap_caps_free(rawWords_);
    sharedFrame_ = nullptr;
    workFrame_ = nullptr;
    filterFrame_ = nullptr;
    scratchFrame_ = nullptr;
    displayFrame_ = nullptr;
    rawWords_ = nullptr;

    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
    appHandle_ = nullptr;
}

void ThermalCamera::onShow(AppHandle app, lv_obj_t* parent) {
    appHandle_ = app;
    root_ = parent;

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    toolbar_ = tt_lvgl_toolbar_create_for_app(parent, app);
    lv_obj_align(toolbar_, LV_ALIGN_TOP_MID, 0, 0);

    content_ = lv_obj_create(parent);
    lv_obj_remove_style_all(content_);
    lv_obj_set_width(content_, LV_PCT(100));
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_layout(content_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content_, uiPad(), 0);
    lv_obj_set_style_pad_row(content_, uiPad(), 0);
    lv_obj_remove_flag(content_, LV_OBJ_FLAG_SCROLLABLE);

    if (imageBuffer_ == nullptr) allocateBuffers();

    showingSettings_ = false;
    buildCameraView();
}

void ThermalCamera::onHide(AppHandle app) {
    if (uiTimer_ != nullptr) {
        lv_timer_delete(uiTimer_);
        uiTimer_ = nullptr;
    }

    // The framework deletes the widget tree that hangs off the parent object.
    root_ = nullptr;
    toolbar_ = nullptr;
    content_ = nullptr;
    imageCanvas_ = nullptr;
    colorBarCanvas_ = nullptr;
    colorBarHighLabel_ = nullptr;
    colorBarLowLabel_ = nullptr;
    spotLabel_ = nullptr;
    spotCaptionLabel_ = nullptr;
    maximumLabel_ = nullptr;
    minimumLabel_ = nullptr;
    averageLabel_ = nullptr;
    regionLabel_ = nullptr;
    detailLabel_ = nullptr;
    statusLabel_ = nullptr;
    paletteButton_ = nullptr;
    rangeButton_ = nullptr;
    measureButton_ = nullptr;
    freezeButton_ = nullptr;
    bindingCount_ = 0;

    settingsSave(settings_);
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

bool ThermalCamera::allocateBuffers() {
    freeBuffers();

    const int pad = uiPad();
    const bool compact = uiIsCompact();
    const int32_t textHeight = static_cast<int32_t>(lvgl_get_text_font_height(uiFont()));
    const int32_t toolbarHeight = textHeight * 2 + 8;
    const int32_t controlHeight = uiButtonHeight() + pad * 2;

    colorBarWidth_ = uiWidth() >= 480 ? 24 : 16;

    int32_t availableWidth = uiWidth() - pad * 4 - colorBarWidth_;
    int32_t availableHeight = uiHeight() - toolbarHeight - controlHeight - pad * 4;

    if (compact) {
        // Readouts sit below the image on narrow screens.
        availableHeight -= textHeight * 3 + pad * 3;
    } else {
        int32_t panelWidth = uiWidth() * 30 / 100;
        if (panelWidth < 150) panelWidth = 150;
        if (panelWidth > 280) panelWidth = 280;
        availableWidth -= panelWidth + pad * 2;
    }

    if (availableWidth < MLX_WIDTH * MINIMUM_SCALE) availableWidth = MLX_WIDTH * MINIMUM_SCALE;
    if (availableHeight < MLX_HEIGHT * MINIMUM_SCALE) availableHeight = MLX_HEIGHT * MINIMUM_SCALE;

    int scale = static_cast<int>(availableWidth / MLX_WIDTH);
    const int verticalScale = static_cast<int>(availableHeight / MLX_HEIGHT);
    if (verticalScale < scale) scale = verticalScale;
    if (scale < MINIMUM_SCALE) scale = MINIMUM_SCALE;
    if (scale > MAXIMUM_SCALE) scale = MAXIMUM_SCALE;

    // Retry with a smaller image when memory is tight; a multiple of the sensor
    // width also guarantees the canvas stride matches our row pitch.
    while (scale >= MINIMUM_SCALE) {
        imageWidth_ = MLX_WIDTH * scale;
        imageHeight_ = MLX_HEIGHT * scale;
        colorBarHeight_ = imageHeight_;

        const size_t imageBytes = static_cast<size_t>(imageWidth_) * static_cast<size_t>(imageHeight_) * 2u;
        const size_t colorBarBytes = static_cast<size_t>(colorBarWidth_) * static_cast<size_t>(colorBarHeight_) * 2u;

        imageBuffer_ = static_cast<uint16_t*>(heap_caps_malloc(imageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (imageBuffer_ == nullptr) {
            imageBuffer_ = static_cast<uint16_t*>(heap_caps_malloc(imageBytes, MALLOC_CAP_8BIT));
        }
        colorBarBuffer_ = static_cast<uint16_t*>(heap_caps_malloc(colorBarBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (colorBarBuffer_ == nullptr) {
            colorBarBuffer_ = static_cast<uint16_t*>(heap_caps_malloc(colorBarBytes, MALLOC_CAP_8BIT));
        }
        columnMap_ = static_cast<ColumnMap*>(
            heap_caps_malloc(sizeof(ColumnMap) * static_cast<size_t>(imageWidth_), MALLOC_CAP_8BIT)
        );

        if (imageBuffer_ != nullptr && colorBarBuffer_ != nullptr && columnMap_ != nullptr) {
            memset(imageBuffer_, 0, imageBytes);
            memset(colorBarBuffer_, 0, colorBarBytes);
            columnMapDirty_ = true;
            ESP_LOGI(TAG, "Image buffer %dx%d (scale %d)", imageWidth_, imageHeight_, scale);
            return true;
        }

        freeBuffers();
        scale--;
    }

    ESP_LOGE(TAG, "Could not allocate an image buffer");
    imageWidth_ = 0;
    imageHeight_ = 0;
    return false;
}

void ThermalCamera::freeBuffers() {
    heap_caps_free(imageBuffer_);
    heap_caps_free(colorBarBuffer_);
    heap_caps_free(columnMap_);
    imageBuffer_ = nullptr;
    colorBarBuffer_ = nullptr;
    columnMap_ = nullptr;
}

void ThermalCamera::rebuildPalette() {
    paletteBuild(settings_.palette, palette_, paletteRgb888_);
    paletteDirty_ = false;
}

void ThermalCamera::rebuildColumnMap() {
    if (columnMap_ == nullptr || imageWidth_ <= 0) return;
    thermalBuildColumnMap(imageWidth_, settings_.mirror, columnMap_);
    columnMapDirty_ = false;
}

// ---------------------------------------------------------------------------
// Sensor acquisition
// ---------------------------------------------------------------------------

bool ThermalCamera::findSensor() {
    SensorSearch search = {MLX_DEFAULT_ADDRESS, nullptr};
    device_for_each_of_type(&I2C_CONTROLLER_TYPE, &search, onI2cController);

    if (search.found == nullptr) {
        i2cDevice_ = nullptr;
        bus_.setTarget(nullptr, MLX_DEFAULT_ADDRESS);
        return false;
    }

    i2cDevice_ = search.found;
    bus_.setTarget(i2cDevice_, MLX_DEFAULT_ADDRESS);
    return true;
}

void ThermalCamera::publishStatus(bool present, bool ready) {
    if (mutex_ == nullptr) return;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;
    sharedStatus_.present = present;
    sharedStatus_.ready = ready;
    sharedStatus_.initStatus = sensor_.getInitStatus();
    sharedStatus_.errorCount = sensor_.getErrorCount();
    xSemaphoreGive(mutex_);
}

void ThermalCamera::publishSettings(bool reconfigure) {
    if (mutex_ == nullptr) return;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) != pdTRUE) return;
    taskSettings_ = settings_;
    if (reconfigure) reconfigureRequested_ = true;
    xSemaphoreGive(mutex_);
}

void ThermalCamera::interruptibleDelay(uint32_t milliseconds) {
    while (milliseconds > 0 && taskRunning_) {
        const uint32_t slice = milliseconds > 50 ? 50 : milliseconds;
        vTaskDelay(pdMS_TO_TICKS(slice));
        milliseconds -= slice;
    }
}

void ThermalCamera::acquisitionTrampoline(void* context) {
    static_cast<ThermalCamera*>(context)->acquisitionLoop();
}

void ThermalCamera::startAcquisition() {
    if (task_ != nullptr) return;
    taskRunning_ = true;
    taskFinished_ = false;
    reconfigureRequested_ = true;

    // 5 kB is comfortable: the frame conversion works on heap buffers.
    if (xTaskCreate(acquisitionTrampoline, "mlx90640", 5120, this, 4, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the acquisition task");
        task_ = nullptr;
        taskRunning_ = false;
        taskFinished_ = true;
    }
}

void ThermalCamera::stopAcquisition() {
    if (task_ == nullptr) return;
    taskRunning_ = false;
    for (int i = 0; i < 300 && !taskFinished_; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    task_ = nullptr;
}

void ThermalCamera::acquisitionLoop() {
    uint8_t subPagesSeen = 0;
    bool filterPrimed = false;
    int64_t previousFrameTime = 0;
    float framesPerSecond = 0.0f;

    while (taskRunning_) {
        CameraSettings local;
        bool reconfigure = false;
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
            local = taskSettings_;
            reconfigure = reconfigureRequested_;
            reconfigureRequested_ = false;
            xSemaphoreGive(mutex_);
        } else {
            continue;
        }

        if (!sensor_.isReady()) {
            const bool present = findSensor();
            const bool ready = present && sensor_.begin(&bus_, MLX_DEFAULT_ADDRESS);
            publishStatus(present, ready);
            if (!ready) {
                if (present) {
                    ESP_LOGW(TAG, "Sensor at 0x%02X did not initialise: %s",
                             MLX_DEFAULT_ADDRESS, mlxInitStatusName(sensor_.getInitStatus()));
                } else {
                    ESP_LOGW(TAG, "No device answering at 0x%02X on any I2C bus", MLX_DEFAULT_ADDRESS);
                }
                interruptibleDelay(1500);
                continue;
            }

            const MlxCalibration& calibration = sensor_.getCalibration();
            ESP_LOGI(TAG, "MLX90640 ready: %u bad pixels, kVdd %.0f, gain %.0f, KtPTAT %.2f%s",
                     static_cast<unsigned>(calibration.badPixelCount),
                     static_cast<double>(calibration.kVdd),
                     static_cast<double>(calibration.gainEE),
                     static_cast<double>(calibration.KtPTAT),
                     sensor_.hasDeviceSelectMismatch() ? " (unexpected device select bit)" : "");
            reconfigure = true;
            subPagesSeen = 0;
            filterPrimed = false;
            memset(workFrame_, 0, MLX_PIXELS * sizeof(float));
        }

        if (reconfigure) {
            sensor_.setResolution(local.resolution);
            sensor_.setPattern(local.pattern);
            sensor_.setRefreshRate(local.refreshRate);
            subPagesSeen = 0;
            filterPrimed = false;
        }

        if (!sensor_.readRawFrame(rawWords_)) {
            if (sensor_.getErrorCount() > 8) {
                ESP_LOGW(TAG, "Lost contact with the sensor, rescanning");
                publishStatus(false, false);
                sensor_.reset(); // forces a fresh begin() on the next pass
                interruptibleDelay(500);
                continue;
            }
            // Not ready yet: poll again well within one sub-page interval.
            uint32_t wait = settingsSubPageIntervalMs(local.refreshRate) / 8;
            if (wait < 5) wait = 5;
            if (wait > 50) wait = 50;
            vTaskDelay(pdMS_TO_TICKS(wait));
            continue;
        }

        MlxFrameInfo info;
        sensor_.calculateTemperatures(
            rawWords_,
            local.emissivity,
            local.reflectedTemperature,
            workFrame_,
            info
        );
        subPagesSeen = static_cast<uint8_t>(subPagesSeen | (1u << info.subPage));
        if (subPagesSeen != 0x03) continue; // the other half of the image is still missing

        sensor_.repairBadPixels(workFrame_);
        if (local.temperatureOffset != 0.0f) {
            for (int i = 0; i < MLX_PIXELS; i++) workFrame_[i] += local.temperatureOffset;
        }

        if (!filterPrimed) {
            memcpy(filterFrame_, workFrame_, MLX_PIXELS * sizeof(float));
            filterPrimed = true;
        } else {
            // A filter setting of 0 keeps the raw frame, 1 averages heavily.
            thermalTemporalFilter(filterFrame_, workFrame_, 1.0f - local.noiseFilter * 0.85f);
        }

        const int64_t now = esp_timer_get_time();
        if (previousFrameTime != 0) {
            const float seconds = static_cast<float>(now - previousFrameTime) / 1000000.0f;
            if (seconds > 0.0005f) {
                const float instant = 1.0f / seconds;
                framesPerSecond = framesPerSecond > 0.0f ? framesPerSecond * 0.7f + instant * 0.3f : instant;
            }
        }
        previousFrameTime = now;

        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
            memcpy(sharedFrame_, filterFrame_, MLX_PIXELS * sizeof(float));
            sharedStatus_.ambientTemperature = info.ambientTemperature;
            sharedStatus_.supplyVoltage = info.supplyVoltage;
            sharedStatus_.framesPerSecond = framesPerSecond;
            sharedStatus_.errorCount = sensor_.getErrorCount();
            sharedStatus_.initStatus = MLX_INIT_OK;
            sharedStatus_.present = true;
            sharedStatus_.ready = true;
            sharedStatus_.frameCounter++;
            xSemaphoreGive(mutex_);
        }
    }

    taskFinished_ = true;
    vTaskDelete(nullptr);
}
