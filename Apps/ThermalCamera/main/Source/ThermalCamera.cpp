#include "ThermalCamera.h"

#include "Ui.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/drivers/i2c_controller.h>

namespace {

constexpr auto* TAG = "ThermalCamera";

/** Words per I2C transfer. Keeps the bus free for the touch controller. */
constexpr size_t I2C_CHUNK_WORDS = 208;
constexpr TickType_t I2C_TIMEOUT = pdMS_TO_TICKS(200);
constexpr TickType_t I2C_PROBE_TIMEOUT = pdMS_TO_TICKS(30);

/** Smallest and largest magnification of the 32x24 array. */
constexpr int MINIMUM_SCALE = 2;
constexpr int MAXIMUM_SCALE = 16;

/** Below this the colour bar is too short to read, so it is left out. */
constexpr int MINIMUM_COLOR_BAR_HEIGHT = 32;

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

void ThermalCamera::start(AppInstanceId appInstanceId) {
    appInstanceId_ = appInstanceId;

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

void ThermalCamera::stop() {
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
}

void ThermalCamera::createWidgets(lv_obj_t* parent) {
    root_ = parent;

    // The window manager deletes this tree when another app takes the screen,
    // without telling us directly. The delete event is that notification: the
    // periodic timer is not part of the tree and would otherwise keep running
    // against freed widgets.
    lv_obj_add_event_cb(parent, onRootDeleted, LV_EVENT_DELETE, this);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    if (imageBuffer_ == nullptr) allocateBuffers();

    buildCameraView();
}

lv_obj_t* ThermalCamera::createContentArea() {
    content_ = lv_obj_create(root_);
    lv_obj_remove_style_all(content_);
    lv_obj_set_width(content_, LV_PCT(100));
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_layout(content_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content_, uiPad(), 0);
    lv_obj_set_style_pad_row(content_, uiPad(), 0);
    lv_obj_remove_flag(content_, LV_OBJ_FLAG_SCROLLABLE);
    return content_;
}

void ThermalCamera::onRootDeleted(lv_event_t* event) {
    static_cast<ThermalCamera*>(lv_event_get_user_data(event))->releaseWidgets();
}

void ThermalCamera::releaseWidgets() {
    if (uiTimer_ != nullptr) {
        lv_timer_delete(uiTimer_);
        uiTimer_ = nullptr;
    }

    // The tree itself is already on its way out; only the references are dropped.
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
    lastStatusMessage_ = nullptr;
    bindingCount_ = 0;
}

void thermalCameraCreateWidgets(lv_obj_t* parent, void* userData) {
    static_cast<ThermalCamera*>(userData)->createWidgets(parent);
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

bool ThermalCamera::allocateBuffers() {
    freeBuffers();

    const int pad = uiPad();
    const bool compact = uiIsCompact();
    const int32_t textHeight = static_cast<int32_t>(lvgl_get_text_font_height(uiFont()));
    const int32_t smallTextHeight = static_cast<int32_t>(lvgl_get_text_font_height(uiSmallFont()));
    const int32_t toolbarHeight = textHeight * 2 + 8;
    const int32_t controlHeight = uiButtonHeight() + pad * 2;

    // A narrow bar reads just as well, and the column has to be wide enough for
    // the span labels above and below it or they spill over the neighbours.
    // The width stays a multiple of eight so that two bytes per pixel land on a
    // row pitch the canvas cannot pad, which would otherwise shear the gradient.
    colorBarWidth_ = uiWidth() >= 480 ? 16 : 8;
    colorBarColumnWidth_ = colorBarWidth_ + smallTextHeight * 2;

    int32_t availableWidth = uiWidth() - pad * 4 - colorBarColumnWidth_;
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

        // The bar shares its column with a label above and one below, so the
        // whole column is only as tall as the image when the bar itself gives
        // both of them room. Sizing the bar to the image made the column two
        // lines taller than the image and pushed the labels out of the layout.
        colorBarHeight_ = imageHeight_ - 2 * (static_cast<int>(smallTextHeight) + COLOR_BAR_GAP);
        if (colorBarHeight_ < MINIMUM_COLOR_BAR_HEIGHT) colorBarHeight_ = 0;

        const size_t imageBytes = static_cast<size_t>(imageWidth_) * static_cast<size_t>(imageHeight_) * 2u;
        const size_t colorBarBytes = static_cast<size_t>(colorBarWidth_) * static_cast<size_t>(colorBarHeight_) * 2u;

        imageBuffer_ = static_cast<uint16_t*>(heap_caps_malloc(imageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (imageBuffer_ == nullptr) {
            imageBuffer_ = static_cast<uint16_t*>(heap_caps_malloc(imageBytes, MALLOC_CAP_8BIT));
        }
        if (colorBarBytes > 0) {
            colorBarBuffer_ = static_cast<uint16_t*>(heap_caps_malloc(colorBarBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (colorBarBuffer_ == nullptr) {
                colorBarBuffer_ = static_cast<uint16_t*>(heap_caps_malloc(colorBarBytes, MALLOC_CAP_8BIT));
            }
        }
        columnMap_ = static_cast<ColumnMap*>(
            heap_caps_malloc(sizeof(ColumnMap) * static_cast<size_t>(imageWidth_), MALLOC_CAP_8BIT)
        );

        const bool colorBarReady = colorBarBytes == 0 || colorBarBuffer_ != nullptr;
        if (imageBuffer_ != nullptr && colorBarReady && columnMap_ != nullptr) {
            memset(imageBuffer_, 0, imageBytes);
            if (colorBarBuffer_ != nullptr) memset(colorBarBuffer_, 0, colorBarBytes);
            columnMapDirty_ = true;
            ESP_LOGI(TAG, "Image %dx%d (scale %d), colour bar %dx%d",
                     imageWidth_, imageHeight_, scale, colorBarWidth_, colorBarHeight_);
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
