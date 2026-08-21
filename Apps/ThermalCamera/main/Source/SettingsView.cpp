/**
 * @file SettingsView.cpp
 * @brief Measurement, image and sensor settings.
 */
#include "ThermalCamera.h"
#include "Ui.h"

#include <cstdio>

namespace {

constexpr const char* INTERPOLATION_OPTIONS = "Off (raw pixels)\nLinear\nSmooth";
constexpr const char* UNIT_OPTIONS = "Celsius\nFahrenheit\nKelvin";
constexpr const char* REFRESH_OPTIONS = "0.5 Hz\n1 Hz\n2 Hz\n4 Hz\n8 Hz\n16 Hz";
constexpr const char* PATTERN_OPTIONS = "Interleaved\nChess";
constexpr const char* RESOLUTION_OPTIONS = "16 bit\n17 bit\n18 bit\n19 bit";
constexpr const char* ALARM_OPTIONS = "Off\nAbove high\nBelow low\nBetween";

/** Fixed point helpers keep slider values integral. */
int toSlider(float value, float scale) {
    return static_cast<int>(value * scale + (value >= 0.0f ? 0.5f : -0.5f));
}

} // namespace

// ---------------------------------------------------------------------------
// Control builders
// ---------------------------------------------------------------------------

lv_obj_t* ThermalCamera::addDropdownSetting(
    lv_obj_t* parent,
    const char* title,
    const char* options,
    int selected,
    SettingKey key
) {
    if (bindingCount_ >= MAX_SETTING_BINDINGS) return nullptr;

    lv_obj_t* row = uiCreateSettingRow(parent, title);
    lv_obj_t* dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(selected < 0 ? 0 : selected));
    lv_obj_set_width(dropdown, uiIsCompact() ? 130 : 190);
    lv_obj_set_style_text_font(dropdown, lvgl_get_text_font(uiSmallFont()), 0);

    SettingBinding& binding = bindings_[bindingCount_++];
    binding.camera = this;
    binding.key = key;
    binding.valueLabel = nullptr;
    binding.format = nullptr;
    binding.displayScale = 1.0f;
    lv_obj_add_event_cb(dropdown, onSettingChanged, LV_EVENT_VALUE_CHANGED, &binding);
    return dropdown;
}

lv_obj_t* ThermalCamera::addSliderSetting(
    lv_obj_t* parent,
    const char* title,
    int minimum,
    int maximum,
    int value,
    SettingKey key,
    const char* format,
    float displayScale
) {
    if (bindingCount_ >= MAX_SETTING_BINDINGS) return nullptr;

    lv_obj_t* row = uiCreateSettingRow(parent, title);

    lv_obj_t* group = uiCreateGroup(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(group, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* slider = lv_slider_create(group);
    lv_slider_set_range(slider, minimum, maximum);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_width(slider, uiIsCompact() ? 96 : 170);

    lv_obj_t* valueLabel = lv_label_create(group);
    lv_obj_set_style_text_font(valueLabel, lvgl_get_text_font(uiSmallFont()), 0);
    lv_obj_set_width(valueLabel, uiIsCompact() ? 46 : 62);
    lv_obj_set_style_text_align(valueLabel, LV_TEXT_ALIGN_RIGHT, 0);

    SettingBinding& binding = bindings_[bindingCount_++];
    binding.camera = this;
    binding.key = key;
    binding.valueLabel = valueLabel;
    binding.format = format;
    binding.displayScale = displayScale;
    updateSettingValueLabel(binding, value);

    lv_obj_add_event_cb(slider, onSettingChanged, LV_EVENT_VALUE_CHANGED, &binding);
    return slider;
}

lv_obj_t* ThermalCamera::addSwitchSetting(lv_obj_t* parent, const char* title, bool value, SettingKey key) {
    if (bindingCount_ >= MAX_SETTING_BINDINGS) return nullptr;

    lv_obj_t* row = uiCreateSettingRow(parent, title);
    lv_obj_t* toggle = lv_switch_create(row);
    if (value) lv_obj_add_state(toggle, LV_STATE_CHECKED);

    SettingBinding& binding = bindings_[bindingCount_++];
    binding.camera = this;
    binding.key = key;
    binding.valueLabel = nullptr;
    binding.format = nullptr;
    binding.displayScale = 1.0f;
    lv_obj_add_event_cb(toggle, onSettingChanged, LV_EVENT_VALUE_CHANGED, &binding);
    return toggle;
}

void ThermalCamera::updateSettingValueLabel(const SettingBinding& binding, int rawValue) {
    if (binding.valueLabel == nullptr || binding.format == nullptr) return;

    char text[24];
    snprintf(
        text,
        sizeof(text),
        binding.format,
        static_cast<double>(static_cast<float>(rawValue) * binding.displayScale)
    );
    lv_label_set_text(binding.valueLabel, text);
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------

void ThermalCamera::buildSettingsView() {
    if (content_ == nullptr) return;
    clearContent();
    showingSettings_ = true;
    refreshToolbar();

    lv_obj_t* list = lv_obj_create(content_);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, uiPad(), 0);
    lv_obj_set_style_pad_row(list, uiPad() / 2, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    // --- Image ------------------------------------------------------------
    uiCreateLabel(list, "IMAGE", uiSmallFont());
    addDropdownSetting(list, "Palette", PALETTE_OPTIONS, settings_.palette, SETTING_PALETTE);
    addDropdownSetting(list, "Interpolation", INTERPOLATION_OPTIONS, settings_.interpolation, SETTING_INTERPOLATION);
    addSwitchSetting(list, "Mirror horizontally", settings_.mirror, SETTING_MIRROR);
    addSwitchSetting(list, "Flip vertically", settings_.flip, SETTING_FLIP);
    addSwitchSetting(list, "Colour bar", settings_.showColorBar, SETTING_COLOR_BAR);
    addSwitchSetting(list, "Crosshair", settings_.showCrosshair, SETTING_CROSSHAIR);

    // --- Temperature span ---------------------------------------------------
    uiCreateLabel(list, "TEMPERATURE SPAN", uiSmallFont());
    addDropdownSetting(list, "Unit", UNIT_OPTIONS, settings_.unit, SETTING_UNIT);
    addSwitchSetting(list, "Automatic span", settings_.autoRange, SETTING_AUTO_RANGE);
    addSliderSetting(
        list,
        "Manual minimum",
        -40,
        300,
        toSlider(settings_.manualRangeMinimum, 1.0f),
        SETTING_RANGE_MINIMUM,
        "%.0fC",
        1.0f
    );
    addSliderSetting(
        list,
        "Manual maximum",
        -40,
        300,
        toSlider(settings_.manualRangeMaximum, 1.0f),
        SETTING_RANGE_MAXIMUM,
        "%.0fC",
        1.0f
    );

    // --- Measurement --------------------------------------------------------
    uiCreateLabel(list, "MEASUREMENT", uiSmallFont());
    addSliderSetting(
        list,
        "Emissivity",
        10,
        100,
        toSlider(settings_.emissivity, 100.0f),
        SETTING_EMISSIVITY,
        "%.2f",
        0.01f
    );
    addSliderSetting(
        list,
        "Reflected temp.",
        -40,
        150,
        toSlider(settings_.reflectedTemperature, 1.0f),
        SETTING_REFLECTED,
        "%.0fC",
        1.0f
    );
    addSliderSetting(
        list,
        "Offset",
        -100,
        100,
        toSlider(settings_.temperatureOffset, 10.0f),
        SETTING_OFFSET,
        "%.1fC",
        0.1f
    );
    addSliderSetting(list, "Noise filter", 0, 100, toSlider(settings_.noiseFilter, 100.0f), SETTING_NOISE, "%.0f%%", 1.0f);

    // --- Alarm --------------------------------------------------------------
    uiCreateLabel(list, "ALARM HIGHLIGHT", uiSmallFont());
    addDropdownSetting(list, "Mode", ALARM_OPTIONS, settings_.alarmMode, SETTING_ALARM_MODE);
    addSliderSetting(list, "Low limit", -40, 300, toSlider(settings_.alarmLow, 1.0f), SETTING_ALARM_LOW, "%.0fC", 1.0f);
    addSliderSetting(
        list,
        "High limit",
        -40,
        300,
        toSlider(settings_.alarmHigh, 1.0f),
        SETTING_ALARM_HIGH,
        "%.0fC",
        1.0f
    );

    // --- Sensor -------------------------------------------------------------
    uiCreateLabel(list, "SENSOR", uiSmallFont());
    addDropdownSetting(list, "Refresh rate", REFRESH_OPTIONS, settings_.refreshRate, SETTING_REFRESH_RATE);
    addDropdownSetting(list, "Read-out pattern", PATTERN_OPTIONS, settings_.pattern, SETTING_PATTERN);
    addDropdownSetting(list, "ADC resolution", RESOLUTION_OPTIONS, settings_.resolution, SETTING_RESOLUTION);

    char info[192];
    const unsigned badPixels = sensor_.getCalibration().badPixelCount;
    snprintf(
        info,
        sizeof(info),
        "%s\nI2C address 0x%02X, %d x %d pixels\nBad pixels: %u\nSensor temperature %.1f C, VDD %.2f V",
        status_.ready ? "MLX90640 connected"
                      : (status_.present ? mlxInitStatusName(status_.initStatus) : "No device at 0x33"),
        MLX_DEFAULT_ADDRESS,
        MLX_WIDTH,
        MLX_HEIGHT,
        badPixels,
        static_cast<double>(status_.ambientTemperature),
        static_cast<double>(status_.supplyVoltage)
    );
    lv_obj_t* infoLabel = uiCreateLabel(list, info, uiSmallFont());
    lv_obj_set_width(infoLabel, LV_PCT(100));
    lv_obj_set_style_text_opa(infoLabel, LV_OPA_70, 0);

    lv_obj_t* resetRow = uiCreateGroup(list, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(resetRow, LV_PCT(100));
    lv_obj_set_height(resetRow, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(resetRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    uiCreateButton(resetRow, "Restore defaults", onResetSettings, this);
}

// ---------------------------------------------------------------------------
// Applying edits
// ---------------------------------------------------------------------------

void ThermalCamera::applySettingFromControl(const SettingBinding& binding, lv_obj_t* control) {
    bool reconfigure = false;
    bool paletteChanged = false;
    bool geometryChanged = false;

    switch (binding.key) {
        case SETTING_PALETTE:
            settings_.palette = static_cast<PaletteId>(lv_dropdown_get_selected(control));
            paletteChanged = true;
            break;
        case SETTING_INTERPOLATION:
            settings_.interpolation = static_cast<InterpolationMode>(lv_dropdown_get_selected(control));
            break;
        case SETTING_UNIT:
            settings_.unit = static_cast<TemperatureUnit>(lv_dropdown_get_selected(control));
            break;
        case SETTING_REFRESH_RATE:
            settings_.refreshRate = static_cast<MlxRefreshRate>(lv_dropdown_get_selected(control));
            reconfigure = true;
            break;
        case SETTING_PATTERN:
            settings_.pattern = static_cast<MlxPattern>(lv_dropdown_get_selected(control));
            reconfigure = true;
            break;
        case SETTING_RESOLUTION:
            settings_.resolution = static_cast<MlxResolution>(lv_dropdown_get_selected(control));
            reconfigure = true;
            break;
        case SETTING_ALARM_MODE:
            settings_.alarmMode = static_cast<AlarmMode>(lv_dropdown_get_selected(control));
            break;

        case SETTING_EMISSIVITY:
            settings_.emissivity = static_cast<float>(lv_slider_get_value(control)) / 100.0f;
            updateSettingValueLabel(binding, lv_slider_get_value(control));
            break;
        case SETTING_REFLECTED:
            settings_.reflectedTemperature = static_cast<float>(lv_slider_get_value(control));
            updateSettingValueLabel(binding, lv_slider_get_value(control));
            break;
        case SETTING_OFFSET:
            settings_.temperatureOffset = static_cast<float>(lv_slider_get_value(control)) / 10.0f;
            updateSettingValueLabel(binding, lv_slider_get_value(control));
            break;
        case SETTING_NOISE:
            settings_.noiseFilter = static_cast<float>(lv_slider_get_value(control)) / 100.0f;
            updateSettingValueLabel(binding, lv_slider_get_value(control));
            break;
        case SETTING_RANGE_MINIMUM:
            settings_.manualRangeMinimum = static_cast<float>(lv_slider_get_value(control));
            updateSettingValueLabel(binding, lv_slider_get_value(control));
            break;
        case SETTING_RANGE_MAXIMUM:
            settings_.manualRangeMaximum = static_cast<float>(lv_slider_get_value(control));
            updateSettingValueLabel(binding, lv_slider_get_value(control));
            break;
        case SETTING_ALARM_LOW:
            settings_.alarmLow = static_cast<float>(lv_slider_get_value(control));
            updateSettingValueLabel(binding, lv_slider_get_value(control));
            break;
        case SETTING_ALARM_HIGH:
            settings_.alarmHigh = static_cast<float>(lv_slider_get_value(control));
            updateSettingValueLabel(binding, lv_slider_get_value(control));
            break;

        case SETTING_MIRROR:
            settings_.mirror = lv_obj_has_state(control, LV_STATE_CHECKED);
            geometryChanged = true;
            break;
        case SETTING_FLIP:
            settings_.flip = lv_obj_has_state(control, LV_STATE_CHECKED);
            break;
        case SETTING_COLOR_BAR:
            settings_.showColorBar = lv_obj_has_state(control, LV_STATE_CHECKED);
            break;
        case SETTING_CROSSHAIR:
            settings_.showCrosshair = lv_obj_has_state(control, LV_STATE_CHECKED);
            break;
        case SETTING_AUTO_RANGE:
            settings_.autoRange = lv_obj_has_state(control, LV_STATE_CHECKED);
            break;
    }

    // Keep the sliders' combined constraints (min below max) consistent.
    settingsValidate(settings_);
    if (paletteChanged) paletteDirty_ = true;
    if (geometryChanged) columnMapDirty_ = true;
    applyAndStoreSettings(reconfigure);
}

void ThermalCamera::applyAndStoreSettings(bool reconfigure) {
    settingsValidate(settings_);
    settingsSave(settings_);
    publishSettings(reconfigure);
}

void ThermalCamera::onSettingChanged(lv_event_t* event) {
    auto* binding = static_cast<SettingBinding*>(lv_event_get_user_data(event));
    if (binding == nullptr || binding->camera == nullptr) return;
    binding->camera->applySettingFromControl(*binding, lv_event_get_target_obj(event));
}

void ThermalCamera::onResetSettings(lv_event_t* event) {
    auto* camera = static_cast<ThermalCamera*>(lv_event_get_user_data(event));
    if (camera == nullptr) return;

    camera->settings_ = CameraSettings();
    camera->paletteDirty_ = true;
    camera->columnMapDirty_ = true;
    camera->applyAndStoreSettings(true);
    camera->rebuildPalette();
    lv_async_call(showSettingsAsync, camera);
}
