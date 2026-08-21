/**
 * @file Palette.h
 * @brief False-colour palettes used to map temperatures to pixels.
 */
#pragma once

#include <cstdint>

/** Number of entries in a generated palette lookup table. */
static constexpr int PALETTE_LEVELS = 256;

enum PaletteId : uint8_t {
    PALETTE_IRON = 0,
    PALETTE_RAINBOW,
    PALETTE_RAINBOW_HC,
    PALETTE_WHITE_HOT,
    PALETTE_BLACK_HOT,
    PALETTE_ARCTIC,
    PALETTE_LAVA,
    PALETTE_AMBER,
    PALETTE_COUNT
};

/** Human readable palette names, indexed by PaletteId. */
extern const char* const PALETTE_NAMES[PALETTE_COUNT];

/** Newline separated palette names, for lv_dropdown_set_options(). */
extern const char* const PALETTE_OPTIONS;

/**
 * @brief Fill a lookup table with the requested palette.
 * @param[in] id the palette to generate
 * @param[out] rgb565 PALETTE_LEVELS entries in the display's native RGB565 layout
 * @param[out] rgb888 PALETTE_LEVELS entries packed as 0x00RRGGBB, used for file export
 */
void paletteBuild(PaletteId id, uint16_t* rgb565, uint32_t* rgb888);

/** Pack an 8-bit RGB triplet into RGB565. */
inline uint16_t paletteToRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
