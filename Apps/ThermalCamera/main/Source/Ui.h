/**
 * @file Ui.h
 * @brief Small helpers that keep the layout readable on every Tactility device.
 *
 * Screens range from 128x128 monochrome panels to 1280x720 tablets, so the
 * layout is driven by the actual display resolution rather than fixed pixels.
 */
#pragma once

#include <lvgl.h>
#include <tactility/lvgl_fonts.h>

inline int32_t uiWidth() { return lv_display_get_horizontal_resolution(nullptr); }

inline int32_t uiHeight() { return lv_display_get_vertical_resolution(nullptr); }

inline int32_t uiShortSide() {
    const int32_t width = uiWidth();
    const int32_t height = uiHeight();
    return width < height ? width : height;
}

/** True on displays too small for a side panel next to the image. */
inline bool uiIsCompact() { return uiWidth() < 480; }

inline int uiPad() {
    const int32_t width = uiWidth();
    if (width < 240) return 2;
    if (width < 400) return 4;
    if (width < 800) return 6;
    return 8;
}

inline LvglFontSize uiFont() {
    const int32_t width = uiWidth();
    if (width < 240) return FONT_SIZE_SMALL;
    if (width < 640) return FONT_SIZE_DEFAULT;
    return FONT_SIZE_LARGE;
}

inline LvglFontSize uiSmallFont() {
    return uiWidth() < 640 ? FONT_SIZE_SMALL : FONT_SIZE_DEFAULT;
}

/** Height of a comfortable touch target. */
inline int32_t uiButtonHeight() {
    const int32_t height = static_cast<int32_t>(lvgl_get_text_font_height(uiFont())) * 2;
    return height < 28 ? 28 : height;
}

/** A borderless, transparent container that only provides layout. */
inline lv_obj_t* uiCreateGroup(lv_obj_t* parent, lv_flex_flow_t flow) {
    lv_obj_t* group = lv_obj_create(parent);
    // Removing every style also clears the theme's size, which would otherwise
    // leave the container at zero by zero pixels.
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(group, flow);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_set_style_pad_row(group, uiPad(), 0);
    lv_obj_set_style_pad_column(group, uiPad(), 0);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    return group;
}

/** A subtly outlined container used to group readouts. */
inline lv_obj_t* uiCreatePanel(lv_obj_t* parent, lv_flex_flow_t flow) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, flow);
    lv_obj_set_style_pad_all(panel, uiPad(), 0);
    lv_obj_set_style_pad_row(panel, uiPad() / 2 + 1, 0);
    lv_obj_set_style_pad_column(panel, uiPad(), 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_30, 0);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    return panel;
}

inline lv_obj_t* uiCreateLabel(lv_obj_t* parent, const char* text, LvglFontSize size) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(size), 0);
    return label;
}

inline lv_obj_t* uiCreateButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, void* userData) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_height(button, uiButtonHeight());
    lv_obj_set_style_pad_hor(button, uiPad() + 2, 0);
    lv_obj_set_style_pad_ver(button, 0, 0);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, userData);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(uiSmallFont()), 0);
    lv_obj_center(label);
    return button;
}

/** Replace the text of a button created with uiCreateButton(). */
inline void uiSetButtonText(lv_obj_t* button, const char* text) {
    if (button == nullptr || lv_obj_get_child_count(button) == 0) return;
    lv_label_set_text(lv_obj_get_child(button, 0), text);
}

/** A settings row: a description on the left and the control on the right. */
inline lv_obj_t* uiCreateSettingRow(lv_obj_t* parent, const char* text) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, uiPad() / 2, 0);
    lv_obj_set_style_pad_column(row, uiPad(), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(uiSmallFont()), 0);
    return row;
}
