/**
 * @file CameraView.cpp
 * @brief The live thermal image, its readouts and the on-screen controls.
 */
#include "Snapshot.h"
#include "ThermalCamera.h"
#include "Ui.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <esp_log.h>
#include <tt_lvgl_toolbar.h>

namespace {

constexpr auto* TAG = "ThermalCamera";

/** Refresh interval of the user interface in milliseconds. */
constexpr uint32_t UI_INTERVAL_MS = 50;

/** How quickly the automatic span follows the scene, per update. */
constexpr float AUTO_RANGE_SMOOTHING = 0.25f;
/** Smallest span the automatic mode will show, so a flat scene stays calm. */
constexpr float AUTO_RANGE_MINIMUM_SPAN = 3.0f;

constexpr uint16_t COLOR_SPOT = 0xFFFF;   // white
constexpr uint16_t COLOR_MAXIMUM = 0xF800; // red
constexpr uint16_t COLOR_MINIMUM = 0x07FF; // cyan
constexpr uint16_t COLOR_BOX = 0xFFE0;     // yellow

const char* measureModeName(MeasureMode mode) {
    switch (mode) {
        case MEASURE_SPOT:
            return "Spot";
        case MEASURE_SPOT_MINMAX:
            return "Spot+MinMax";
        case MEASURE_BOX:
        default:
            return "Box";
    }
}

void formatTemperature(char* buffer, size_t size, float celsius, TemperatureUnit unit) {
    snprintf(buffer, size, "%.1f%s", static_cast<double>(thermalConvertUnit(celsius, unit)), thermalUnitSuffix(unit));
}

} // namespace

// ---------------------------------------------------------------------------
// View construction
// ---------------------------------------------------------------------------

void ThermalCamera::clearContent() {
    if (uiTimer_ != nullptr) {
        lv_timer_delete(uiTimer_);
        uiTimer_ = nullptr;
    }
    if (content_ != nullptr) lv_obj_clean(content_);

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

void ThermalCamera::refreshToolbar() {
    if (toolbar_ == nullptr) return;
    tt_lvgl_toolbar_clear_actions(toolbar_);

    if (showingSettings_) {
        tt_lvgl_toolbar_set_title(toolbar_, "Settings");
        tt_lvgl_toolbar_add_text_button_action(toolbar_, LV_SYMBOL_LEFT, onBackButton, this);
    } else {
        tt_lvgl_toolbar_set_title(toolbar_, "Thermal Camera");
        tt_lvgl_toolbar_add_text_button_action(toolbar_, LV_SYMBOL_SAVE, onSaveButton, this);
        tt_lvgl_toolbar_add_text_button_action(toolbar_, LV_SYMBOL_SETTINGS, onSettingsButton, this);
    }
}

void ThermalCamera::buildCameraView() {
    if (content_ == nullptr) return;
    clearContent();
    showingSettings_ = false;
    refreshToolbar();

    const int pad = uiPad();
    const bool compact = uiIsCompact();

    lv_obj_t* mainRow = uiCreateGroup(content_, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(mainRow, LV_PCT(100));
    lv_obj_set_flex_grow(mainRow, 1);
    lv_obj_set_flex_align(mainRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // --- Thermal image --------------------------------------------------
    if (imageBuffer_ != nullptr && imageWidth_ > 0) {
        imageCanvas_ = lv_canvas_create(mainRow);
        lv_canvas_set_buffer(imageCanvas_, imageBuffer_, imageWidth_, imageHeight_, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(imageCanvas_, imageWidth_, imageHeight_);
        lv_obj_set_style_border_width(imageCanvas_, 1, 0);
        lv_obj_set_style_border_opa(imageCanvas_, LV_OPA_50, 0);
        lv_obj_add_flag(imageCanvas_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(imageCanvas_, onImagePressed, LV_EVENT_PRESSING, this);
        lv_obj_add_event_cb(imageCanvas_, onImagePressed, LV_EVENT_CLICKED, this);
    } else {
        lv_obj_t* placeholder = uiCreateLabel(mainRow, "No image buffer", uiFont());
        lv_obj_set_flex_grow(placeholder, 1);
    }

    // --- Colour bar ------------------------------------------------------
    if (settings_.showColorBar && colorBarBuffer_ != nullptr) {
        // The column is content sized vertically, so its main axis alignment has
        // to stay START; the labels are centred through their own text align.
        lv_obj_t* colorBarGroup = uiCreateGroup(mainRow, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(colorBarGroup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(colorBarGroup, 2, 0);
        lv_obj_set_size(colorBarGroup, colorBarColumnWidth_, LV_SIZE_CONTENT);

        colorBarHighLabel_ = uiCreateLabel(colorBarGroup, "--", uiSmallFont());
        lv_obj_set_width(colorBarHighLabel_, colorBarColumnWidth_);
        lv_obj_set_style_text_align(colorBarHighLabel_, LV_TEXT_ALIGN_CENTER, 0);
        colorBarCanvas_ = lv_canvas_create(colorBarGroup);
        lv_canvas_set_buffer(
            colorBarCanvas_,
            colorBarBuffer_,
            colorBarWidth_,
            colorBarHeight_,
            LV_COLOR_FORMAT_RGB565
        );
        lv_obj_set_size(colorBarCanvas_, colorBarWidth_, colorBarHeight_);
        lv_obj_set_style_border_width(colorBarCanvas_, 1, 0);
        lv_obj_set_style_border_opa(colorBarCanvas_, LV_OPA_50, 0);
        colorBarLowLabel_ = uiCreateLabel(colorBarGroup, "--", uiSmallFont());
        lv_obj_set_width(colorBarLowLabel_, colorBarColumnWidth_);
        lv_obj_set_style_text_align(colorBarLowLabel_, LV_TEXT_ALIGN_CENTER, 0);
        updateColorBar();
    }

    // --- Readouts --------------------------------------------------------
    lv_obj_t* infoParent = compact ? content_ : mainRow;
    lv_obj_t* info = uiCreatePanel(infoParent, LV_FLEX_FLOW_COLUMN);
    if (compact) {
        lv_obj_set_width(info, LV_PCT(100));
        lv_obj_set_height(info, LV_SIZE_CONTENT);
    } else {
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_height(info, LV_PCT(100));
    }
    lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_AUTO);

    spotCaptionLabel_ = uiCreateLabel(info, "SPOT", uiSmallFont());
    lv_obj_set_style_text_opa(spotCaptionLabel_, LV_OPA_70, 0);

    spotLabel_ = uiCreateLabel(info, "--.-", FONT_SIZE_LARGE);

    lv_obj_t* extremes = uiCreateGroup(info, compact ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(extremes, LV_PCT(100));
    lv_obj_set_height(extremes, LV_SIZE_CONTENT);
    // Spreading only works along an axis with a definite size. Stacked in a
    // column the height is content sized, so the rows have to start at the top.
    lv_obj_set_flex_align(
        extremes,
        compact ? LV_FLEX_ALIGN_SPACE_BETWEEN : LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START
    );
    lv_obj_set_style_pad_row(extremes, uiPad() / 2 + 1, 0);

    maximumLabel_ = uiCreateLabel(extremes, "Max --.-", uiFont());
    lv_obj_set_style_text_color(maximumLabel_, lv_color_hex(0xFF6B4A), 0);
    minimumLabel_ = uiCreateLabel(extremes, "Min --.-", uiFont());
    lv_obj_set_style_text_color(minimumLabel_, lv_color_hex(0x5AC8FA), 0);
    averageLabel_ = uiCreateLabel(extremes, "Avg --.-", uiFont());

    if (compact) {
        // Side by side the three readouts share the width evenly, so a wide
        // reading shrinks its neighbours rather than overlapping them.
        lv_obj_set_flex_grow(maximumLabel_, 1);
        lv_obj_set_flex_grow(minimumLabel_, 1);
        lv_obj_set_flex_grow(averageLabel_, 1);
    }

    regionLabel_ = uiCreateLabel(info, "", uiSmallFont());
    lv_obj_set_width(regionLabel_, LV_PCT(100));

    detailLabel_ = uiCreateLabel(info, "", uiSmallFont());
    lv_obj_set_width(detailLabel_, LV_PCT(100));
    lv_obj_set_style_text_opa(detailLabel_, LV_OPA_80, 0);

    statusLabel_ = uiCreateLabel(info, "Looking for MLX90640...", uiSmallFont());
    lv_obj_set_width(statusLabel_, LV_PCT(100));
    lv_obj_set_style_text_color(statusLabel_, lv_color_hex(0xFFC24A), 0);

    // --- Controls --------------------------------------------------------
    lv_obj_t* controls = uiCreateGroup(content_, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(controls, LV_PCT(100));
    lv_obj_set_height(controls, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(controls, pad, 0);

    paletteButton_ = uiCreateButton(controls, PALETTE_NAMES[settings_.palette], onPaletteButton, this);
    rangeButton_ = uiCreateButton(controls, "Auto", onRangeButton, this);
    measureButton_ = uiCreateButton(controls, measureModeName(settings_.measureMode), onMeasureButton, this);
    freezeButton_ = uiCreateButton(controls, "Freeze", onFreezeButton, this);

    updateControlLabels();

    uiTimer_ = lv_timer_create(onUiTimer, UI_INTERVAL_MS, this);

    // Show whatever the acquisition task already produced. fetchFrame() only
    // reports new frames, so a rebuilt view redraws from the frame it kept.
    fetchFrame();
    if (haveFrame_) renderImage();
    updateReadouts();
}

// ---------------------------------------------------------------------------
// Periodic update
// ---------------------------------------------------------------------------

void ThermalCamera::onUiTimer(lv_timer_t* timer) {
    static_cast<ThermalCamera*>(lv_timer_get_user_data(timer))->onUiTick();
}

void ThermalCamera::onUiTick() {
    if (showingSettings_) return;

    // fetchFrame() refreshes the sensor status on every call but only reports
    // true for a new image, so repainting stays tied to the frame rate.
    if (fetchFrame()) {
        renderImage();
        updateReadouts();
    } else if (!status_.ready) {
        updateReadouts();
    }
}

bool ThermalCamera::fetchFrame() {
    if (mutex_ == nullptr || sharedFrame_ == nullptr || displayFrame_ == nullptr) return false;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    const bool isNew = sharedStatus_.frameCounter != lastFrameCounter_;
    status_ = sharedStatus_;
    if (isNew && !frozen_) {
        memcpy(displayFrame_, sharedFrame_, MLX_PIXELS * sizeof(float));
        lastFrameCounter_ = sharedStatus_.frameCounter;
        haveFrame_ = true;
    } else if (isNew) {
        // Keep the counter in step so unfreezing shows the next frame straight away.
        lastFrameCounter_ = sharedStatus_.frameCounter;
    }
    xSemaphoreGive(mutex_);

    if (!isNew || frozen_ || !haveFrame_) return false;

    thermalComputeStats(displayFrame_, stats_);
    if (settings_.measureMode == MEASURE_BOX) {
        thermalComputeRegionStats(
            displayFrame_,
            settings_.boxLeft,
            settings_.boxTop,
            settings_.boxRight,
            settings_.boxBottom,
            regionStats_
        );
    }

    if (settings_.autoRange) {
        float low = stats_.minimum;
        float high = stats_.maximum;
        const float span = high - low;
        if (span < AUTO_RANGE_MINIMUM_SPAN) {
            const float centre = (high + low) * 0.5f;
            low = centre - AUTO_RANGE_MINIMUM_SPAN * 0.5f;
            high = centre + AUTO_RANGE_MINIMUM_SPAN * 0.5f;
        }
        // Smooth the span so the colours do not jump on every frame.
        displayRangeMinimum_ += (low - displayRangeMinimum_) * AUTO_RANGE_SMOOTHING;
        displayRangeMaximum_ += (high - displayRangeMaximum_) * AUTO_RANGE_SMOOTHING;
    } else {
        displayRangeMinimum_ = settings_.manualRangeMinimum;
        displayRangeMaximum_ = settings_.manualRangeMaximum;
    }

    return true;
}

void ThermalCamera::renderImage() {
    if (imageCanvas_ == nullptr || imageBuffer_ == nullptr || !haveFrame_) return;
    if (paletteDirty_) rebuildPalette();
    if (columnMapDirty_) rebuildColumnMap();
    if (columnMap_ == nullptr) return;

    const float* source = displayFrame_;
    if (settings_.interpolation == INTERP_SMOOTH && scratchFrame_ != nullptr) {
        thermalSpatialFilter(displayFrame_, scratchFrame_);
        source = scratchFrame_;
    }

    RenderOptions options;
    options.palette = palette_;
    options.rangeMinimum = displayRangeMinimum_;
    options.rangeMaximum = displayRangeMaximum_;
    options.interpolation = settings_.interpolation;
    options.mirror = settings_.mirror;
    options.flip = settings_.flip;
    options.alarmMode = settings_.alarmMode;
    options.alarmLow = settings_.alarmLow;
    options.alarmHigh = settings_.alarmHigh;
    options.alarmColor = paletteToRgb565(255, 0, 255);

    thermalRender(source, imageBuffer_, imageWidth_, imageHeight_, options, columnMap_);

    const int arm = imageWidth_ / 24 + 4;
    const int markerSize = imageWidth_ / 90 + 2;
    int x = 0;
    int y = 0;

    if (settings_.measureMode == MEASURE_BOX) {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        thermalSensorToOutput(
            settings_.boxTop * MLX_WIDTH + settings_.boxLeft,
            imageWidth_,
            imageHeight_,
            settings_.mirror,
            settings_.flip,
            left,
            top
        );
        thermalSensorToOutput(
            settings_.boxBottom * MLX_WIDTH + settings_.boxRight,
            imageWidth_,
            imageHeight_,
            settings_.mirror,
            settings_.flip,
            right,
            bottom
        );
        thermalDrawRect(imageBuffer_, imageWidth_, imageHeight_, left, top, right, bottom, COLOR_BOX);

        thermalSensorToOutput(
            regionStats_.maximumIndex, imageWidth_, imageHeight_, settings_.mirror, settings_.flip, x, y
        );
        thermalDrawMarker(imageBuffer_, imageWidth_, imageHeight_, x, y, markerSize, COLOR_MAXIMUM);
        thermalSensorToOutput(
            regionStats_.minimumIndex, imageWidth_, imageHeight_, settings_.mirror, settings_.flip, x, y
        );
        thermalDrawMarker(imageBuffer_, imageWidth_, imageHeight_, x, y, markerSize, COLOR_MINIMUM);
    } else if (settings_.measureMode == MEASURE_SPOT_MINMAX) {
        thermalSensorToOutput(stats_.maximumIndex, imageWidth_, imageHeight_, settings_.mirror, settings_.flip, x, y);
        thermalDrawMarker(imageBuffer_, imageWidth_, imageHeight_, x, y, markerSize, COLOR_MAXIMUM);
        thermalSensorToOutput(stats_.minimumIndex, imageWidth_, imageHeight_, settings_.mirror, settings_.flip, x, y);
        thermalDrawMarker(imageBuffer_, imageWidth_, imageHeight_, x, y, markerSize, COLOR_MINIMUM);
    }

    if (settings_.showCrosshair && settings_.measureMode != MEASURE_BOX) {
        thermalSensorToOutput(settings_.spotIndex, imageWidth_, imageHeight_, settings_.mirror, settings_.flip, x, y);
        thermalDrawCrosshair(imageBuffer_, imageWidth_, imageHeight_, x, y, arm, COLOR_SPOT);
    }

    lv_obj_invalidate(imageCanvas_);
}

void ThermalCamera::updateColorBar() {
    if (colorBarCanvas_ == nullptr || colorBarBuffer_ == nullptr) return;
    if (paletteDirty_) rebuildPalette();
    thermalRenderColorBar(palette_, colorBarBuffer_, colorBarWidth_, colorBarHeight_);
    lv_obj_invalidate(colorBarCanvas_);
}

void ThermalCamera::updateReadouts() {
    char buffer[96];
    const TemperatureUnit unit = settings_.unit;

    if (!status_.ready) {
        // Naming the failure saves guessing between a wiring fault, a bus fault
        // and a sensor that answers but cannot be calibrated.
        const char* message = status_.present ? mlxInitStatusName(status_.initStatus)
                                              : "no device at 0x33 on any I2C bus";
        if (message != lastStatusMessage_) {
            if (spotLabel_ != nullptr) lv_label_set_text(spotLabel_, "--.-");
            if (statusLabel_ != nullptr) {
                snprintf(buffer, sizeof(buffer), "MLX90640: %s", message);
                lv_label_set_text(statusLabel_, buffer);
            }
            lastStatusMessage_ = message;
        }
        return;
    }
    lastStatusMessage_ = nullptr;

    if (statusLabel_ != nullptr) {
        lv_label_set_text(statusLabel_, statusText_[0] != '\0' ? statusText_ : (frozen_ ? "Image frozen" : ""));
    }

    if (!haveFrame_) return;

    if (settings_.measureMode == MEASURE_BOX) {
        if (spotCaptionLabel_ != nullptr) lv_label_set_text(spotCaptionLabel_, "BOX AVERAGE");
        formatTemperature(buffer, sizeof(buffer), regionStats_.average, unit);
        if (spotLabel_ != nullptr) lv_label_set_text(spotLabel_, buffer);

        snprintf(
            buffer,
            sizeof(buffer),
            "Box %d-%d x %d-%d, %d px",
            settings_.boxLeft,
            settings_.boxRight,
            settings_.boxTop,
            settings_.boxBottom,
            regionStats_.pixelCount
        );
        if (regionLabel_ != nullptr) lv_label_set_text(regionLabel_, buffer);
    } else {
        if (spotCaptionLabel_ != nullptr) lv_label_set_text(spotCaptionLabel_, "SPOT");
        const float spot = displayFrame_[settings_.spotIndex];
        formatTemperature(buffer, sizeof(buffer), spot, unit);
        if (spotLabel_ != nullptr) lv_label_set_text(spotLabel_, buffer);
        if (regionLabel_ != nullptr) lv_label_set_text(regionLabel_, "");
    }

    const FrameStats& shown = stats_;
    const float maximum = settings_.measureMode == MEASURE_BOX ? regionStats_.maximum : shown.maximum;
    const float minimum = settings_.measureMode == MEASURE_BOX ? regionStats_.minimum : shown.minimum;

    char value[24];
    formatTemperature(value, sizeof(value), maximum, unit);
    snprintf(buffer, sizeof(buffer), "Max %s", value);
    if (maximumLabel_ != nullptr) lv_label_set_text(maximumLabel_, buffer);

    formatTemperature(value, sizeof(value), minimum, unit);
    snprintf(buffer, sizeof(buffer), "Min %s", value);
    if (minimumLabel_ != nullptr) lv_label_set_text(minimumLabel_, buffer);

    formatTemperature(value, sizeof(value), shown.average, unit);
    snprintf(buffer, sizeof(buffer), "Avg %s", value);
    if (averageLabel_ != nullptr) lv_label_set_text(averageLabel_, buffer);

    if (detailLabel_ != nullptr) {
        snprintf(
            buffer,
            sizeof(buffer),
            "Range %.1f..%.1f%s\nE=%.2f  Tamb %.1fC\n%.1f fps  %s",
            static_cast<double>(thermalConvertUnit(displayRangeMinimum_, unit)),
            static_cast<double>(thermalConvertUnit(displayRangeMaximum_, unit)),
            thermalUnitSuffix(unit),
            static_cast<double>(settings_.emissivity),
            static_cast<double>(status_.ambientTemperature),
            static_cast<double>(status_.framesPerSecond),
            settings_.autoRange ? "auto" : "manual"
        );
        lv_label_set_text(detailLabel_, buffer);
    }

    // The unit already appears in the detail line, so the bar carries bare
    // numbers and stays inside its narrow column.
    if (colorBarHighLabel_ != nullptr) {
        snprintf(value, sizeof(value), "%.0f", static_cast<double>(thermalConvertUnit(displayRangeMaximum_, unit)));
        lv_label_set_text(colorBarHighLabel_, value);
    }
    if (colorBarLowLabel_ != nullptr) {
        snprintf(value, sizeof(value), "%.0f", static_cast<double>(thermalConvertUnit(displayRangeMinimum_, unit)));
        lv_label_set_text(colorBarLowLabel_, value);
    }
}

void ThermalCamera::updateControlLabels() {
    if (paletteButton_ != nullptr) uiSetButtonText(paletteButton_, PALETTE_NAMES[settings_.palette]);
    if (rangeButton_ != nullptr) uiSetButtonText(rangeButton_, settings_.autoRange ? "Auto" : "Manual");
    if (measureButton_ != nullptr) uiSetButtonText(measureButton_, measureModeName(settings_.measureMode));
    if (freezeButton_ != nullptr) uiSetButtonText(freezeButton_, frozen_ ? "Live" : "Freeze");
}

void ThermalCamera::setStatusMessage(const char* message) {
    if (message == nullptr) {
        statusText_[0] = '\0';
    } else {
        snprintf(statusText_, sizeof(statusText_), "%s", message);
    }
    if (statusLabel_ != nullptr) lv_label_set_text(statusLabel_, statusText_);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void ThermalCamera::handleImageTouch(int32_t x, int32_t y) {
    if (imageWidth_ <= 0 || imageHeight_ <= 0) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= imageWidth_) x = imageWidth_ - 1;
    if (y >= imageHeight_) y = imageHeight_ - 1;

    const int sensorIndex = thermalOutputToSensor(
        static_cast<int>(x),
        static_cast<int>(y),
        imageWidth_,
        imageHeight_,
        settings_.mirror,
        settings_.flip
    );

    if (settings_.measureMode != MEASURE_BOX && sensorIndex == settings_.spotIndex) return;

    if (settings_.measureMode == MEASURE_BOX) {
        // Re-centre the box on the touched pixel, keeping its size.
        const int halfWidth = (settings_.boxRight - settings_.boxLeft) / 2;
        const int halfHeight = (settings_.boxBottom - settings_.boxTop) / 2;
        const int centreX = sensorIndex % MLX_WIDTH;
        const int centreY = sensorIndex / MLX_WIDTH;
        settings_.boxLeft = centreX - halfWidth;
        settings_.boxRight = centreX + halfWidth;
        settings_.boxTop = centreY - halfHeight;
        settings_.boxBottom = centreY + halfHeight;
        settingsValidate(settings_);
    } else {
        settings_.spotIndex = sensorIndex;
    }

    if (haveFrame_) {
        if (settings_.measureMode == MEASURE_BOX) {
            thermalComputeRegionStats(
                displayFrame_,
                settings_.boxLeft,
                settings_.boxTop,
                settings_.boxRight,
                settings_.boxBottom,
                regionStats_
            );
        }
        renderImage();
        updateReadouts();
    }
}

void ThermalCamera::saveSnapshot() {
    if (!haveFrame_ || imageBuffer_ == nullptr || appHandle_ == nullptr) {
        setStatusMessage("Nothing to save yet");
        return;
    }

    setStatusMessage("Saving...");
    lv_refr_now(nullptr);

    char directory[256];
    size_t size = sizeof(directory);
    tt_app_get_user_data_path(appHandle_, directory, &size);

    // Create every missing level of the user data directory.
    for (char* cursor = directory + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        mkdir(directory, 0755);
        *cursor = '/';
    }
    mkdir(directory, 0755);

    char base[32];
    snapshotMakeBaseName(base, sizeof(base));

    char child[64];
    char path[320];
    size = sizeof(path);
    snprintf(child, sizeof(child), "%s.bmp", base);
    tt_app_get_user_data_child_path(appHandle_, child, path, &size);
    const bool imageSaved = snapshotWriteBmp(path, imageBuffer_, imageWidth_, imageHeight_);

    SnapshotMetadata metadata;
    metadata.emissivity = settings_.emissivity;
    metadata.reflectedTemperature = settings_.reflectedTemperature;
    metadata.ambientTemperature = status_.ambientTemperature;
    metadata.minimum = stats_.minimum;
    metadata.maximum = stats_.maximum;
    metadata.average = stats_.average;
    metadata.paletteName = PALETTE_NAMES[settings_.palette];

    size = sizeof(path);
    snprintf(child, sizeof(child), "%s.csv", base);
    tt_app_get_user_data_child_path(appHandle_, child, path, &size);
    const bool dataSaved = snapshotWriteCsv(path, displayFrame_, metadata);

    char message[80];
    if (imageSaved && dataSaved) {
        snprintf(message, sizeof(message), "Saved %s.bmp + .csv", base);
    } else if (imageSaved || dataSaved) {
        snprintf(message, sizeof(message), "Partly saved %s", base);
    } else {
        snprintf(message, sizeof(message), "Save failed (no storage?)");
    }
    ESP_LOGI(TAG, "%s", message);
    setStatusMessage(message);
}

// ---------------------------------------------------------------------------
// LVGL callbacks
// ---------------------------------------------------------------------------

void ThermalCamera::onImagePressed(lv_event_t* event) {
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    lv_obj_t* canvas = lv_event_get_target_obj(event);
    if (camera == nullptr || canvas == nullptr) return;

    lv_indev_t* input = lv_indev_active();
    if (input == nullptr) return;

    lv_point_t point;
    lv_indev_get_point(input, &point);

    lv_area_t area;
    lv_obj_get_coords(canvas, &area);
    camera->handleImageTouch(point.x - area.x1, point.y - area.y1);
}

void ThermalCamera::onPaletteButton(lv_event_t* event) {
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    if (camera == nullptr) return;

    camera->settings_.palette =
        static_cast<PaletteId>((camera->settings_.palette + 1) % PALETTE_COUNT);
    camera->rebuildPalette();
    camera->updateColorBar();
    camera->updateControlLabels();
    camera->renderImage();
    camera->applyAndStoreSettings(false);
}

void ThermalCamera::onRangeButton(lv_event_t* event) {
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    if (camera == nullptr) return;

    if (camera->settings_.autoRange && camera->haveFrame_) {
        // Leaving automatic mode keeps the span that is currently on screen.
        camera->settings_.manualRangeMinimum = camera->displayRangeMinimum_;
        camera->settings_.manualRangeMaximum = camera->displayRangeMaximum_;
        settingsValidate(camera->settings_);
    }
    camera->settings_.autoRange = !camera->settings_.autoRange;
    camera->updateControlLabels();
    camera->updateReadouts();
    camera->applyAndStoreSettings(false);
}

void ThermalCamera::onMeasureButton(lv_event_t* event) {
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    if (camera == nullptr) return;

    camera->settings_.measureMode =
        static_cast<MeasureMode>((camera->settings_.measureMode + 1) % (MEASURE_BOX + 1));
    if (camera->settings_.measureMode == MEASURE_BOX && camera->haveFrame_) {
        thermalComputeRegionStats(
            camera->displayFrame_,
            camera->settings_.boxLeft,
            camera->settings_.boxTop,
            camera->settings_.boxRight,
            camera->settings_.boxBottom,
            camera->regionStats_
        );
    }
    camera->updateControlLabels();
    camera->renderImage();
    camera->updateReadouts();
    camera->applyAndStoreSettings(false);
}

void ThermalCamera::onFreezeButton(lv_event_t* event) {
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    if (camera == nullptr) return;

    camera->frozen_ = !camera->frozen_;
    camera->updateControlLabels();
    camera->setStatusMessage(camera->frozen_ ? "Image frozen" : nullptr);
}

void ThermalCamera::onSaveButton(lv_event_t* event) {
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    if (camera != nullptr) camera->saveSnapshot();
}

void ThermalCamera::showCameraAsync(void* context) {
    static_cast<ThermalCamera*>(context)->buildCameraView();
}

void ThermalCamera::showSettingsAsync(void* context) {
    static_cast<ThermalCamera*>(context)->buildSettingsView();
}

void ThermalCamera::onSettingsButton(lv_event_t* event) {
    // Switching views deletes the toolbar button that is handling this event.
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    if (camera != nullptr) lv_async_call(showSettingsAsync, camera);
}

void ThermalCamera::onBackButton(lv_event_t* event) {
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    if (camera != nullptr) lv_async_call(showCameraAsync, camera);
}
