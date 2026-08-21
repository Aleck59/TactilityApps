#include "Palette.h"

namespace {

struct Stop {
    float position;
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct PaletteDefinition {
    const Stop* stops;
    int count;
};

// Ironbow: the classic thermographic palette. Dark purple at the cold end
// through red and orange to white at the hot end.
constexpr Stop IRON_STOPS[] = {
    {0.00f, 0, 0, 12},
    {0.10f, 28, 0, 80},
    {0.22f, 80, 0, 130},
    {0.36f, 140, 12, 130},
    {0.50f, 190, 40, 90},
    {0.62f, 226, 78, 32},
    {0.75f, 248, 128, 0},
    {0.86f, 255, 180, 0},
    {0.95f, 255, 226, 90},
    {1.00f, 255, 255, 255}
};

// Standard rainbow: highest perceived contrast for small temperature deltas.
constexpr Stop RAINBOW_STOPS[] = {
    {0.00f, 0, 0, 60},
    {0.15f, 0, 0, 220},
    {0.32f, 0, 190, 255},
    {0.50f, 0, 220, 60},
    {0.67f, 235, 235, 0},
    {0.84f, 255, 110, 0},
    {1.00f, 255, 0, 0}
};

// High contrast rainbow with black and white end caps, so under- and
// over-range areas stay recognisable in a locked temperature span.
constexpr Stop RAINBOW_HC_STOPS[] = {
    {0.00f, 0, 0, 0},
    {0.08f, 20, 0, 140},
    {0.22f, 0, 60, 240},
    {0.36f, 0, 200, 220},
    {0.50f, 0, 210, 60},
    {0.64f, 220, 230, 0},
    {0.78f, 255, 130, 0},
    {0.90f, 255, 20, 20},
    {1.00f, 255, 255, 255}
};

constexpr Stop WHITE_HOT_STOPS[] = {
    {0.00f, 0, 0, 0},
    {1.00f, 255, 255, 255}
};

constexpr Stop BLACK_HOT_STOPS[] = {
    {0.00f, 255, 255, 255},
    {1.00f, 0, 0, 0}
};

// Arctic: cold scene in blue, warm targets highlighted in yellow and white.
constexpr Stop ARCTIC_STOPS[] = {
    {0.00f, 0, 8, 40},
    {0.30f, 0, 60, 130},
    {0.55f, 40, 140, 190},
    {0.75f, 160, 200, 220},
    {0.88f, 255, 220, 120},
    {1.00f, 255, 255, 230}
};

constexpr Stop LAVA_STOPS[] = {
    {0.00f, 0, 0, 0},
    {0.35f, 90, 0, 0},
    {0.60f, 200, 30, 0},
    {0.80f, 255, 120, 0},
    {0.93f, 255, 210, 60},
    {1.00f, 255, 255, 220}
};

constexpr Stop AMBER_STOPS[] = {
    {0.00f, 8, 4, 0},
    {0.45f, 120, 55, 0},
    {0.75f, 220, 130, 10},
    {1.00f, 255, 232, 170}
};

constexpr PaletteDefinition DEFINITIONS[PALETTE_COUNT] = {
    {IRON_STOPS, static_cast<int>(sizeof(IRON_STOPS) / sizeof(Stop))},
    {RAINBOW_STOPS, static_cast<int>(sizeof(RAINBOW_STOPS) / sizeof(Stop))},
    {RAINBOW_HC_STOPS, static_cast<int>(sizeof(RAINBOW_HC_STOPS) / sizeof(Stop))},
    {WHITE_HOT_STOPS, static_cast<int>(sizeof(WHITE_HOT_STOPS) / sizeof(Stop))},
    {BLACK_HOT_STOPS, static_cast<int>(sizeof(BLACK_HOT_STOPS) / sizeof(Stop))},
    {ARCTIC_STOPS, static_cast<int>(sizeof(ARCTIC_STOPS) / sizeof(Stop))},
    {LAVA_STOPS, static_cast<int>(sizeof(LAVA_STOPS) / sizeof(Stop))},
    {AMBER_STOPS, static_cast<int>(sizeof(AMBER_STOPS) / sizeof(Stop))}
};

uint8_t interpolateChannel(uint8_t from, uint8_t to, float weight) {
    const float value = static_cast<float>(from) + weight * (static_cast<float>(to) - static_cast<float>(from));
    if (value <= 0.0f) return 0;
    if (value >= 255.0f) return 255;
    return static_cast<uint8_t>(value + 0.5f);
}

} // namespace

const char* const PALETTE_NAMES[PALETTE_COUNT] = {
    "Iron",
    "Rainbow",
    "Rainbow HC",
    "White hot",
    "Black hot",
    "Arctic",
    "Lava",
    "Amber"
};

const char* const PALETTE_OPTIONS = "Iron\nRainbow\nRainbow HC\nWhite hot\nBlack hot\nArctic\nLava\nAmber";

void paletteBuild(PaletteId id, uint16_t* rgb565, uint32_t* rgb888) {
    if (id >= PALETTE_COUNT) id = PALETTE_IRON;
    const PaletteDefinition& definition = DEFINITIONS[id];

    int segment = 0;
    for (int i = 0; i < PALETTE_LEVELS; i++) {
        const float position = static_cast<float>(i) / static_cast<float>(PALETTE_LEVELS - 1);

        while (segment < definition.count - 2 && position > definition.stops[segment + 1].position) {
            segment++;
        }

        const Stop& from = definition.stops[segment];
        const Stop& to = definition.stops[segment + 1];
        const float span = to.position - from.position;
        float weight = (span > 0.0f) ? (position - from.position) / span : 0.0f;
        if (weight < 0.0f) weight = 0.0f;
        if (weight > 1.0f) weight = 1.0f;

        const uint8_t r = interpolateChannel(from.r, to.r, weight);
        const uint8_t g = interpolateChannel(from.g, to.g, weight);
        const uint8_t b = interpolateChannel(from.b, to.b, weight);

        if (rgb565 != nullptr) rgb565[i] = paletteToRgb565(r, g, b);
        if (rgb888 != nullptr) {
            rgb888[i] = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
        }
    }
}
