/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// The accent colour and the neutrals that carry its tint. Picking another
// accent keeps each neutral's lightness and moves its hue, so the panels,
// rules and dim text follow the accent instead of staying green.
namespace cp0::theme {

struct Palette {
    uint32_t accent;
    uint32_t dim;
    uint32_t select;
    uint32_t line;
    uint32_t panel;
    uint32_t bracket;
    uint32_t track;
    uint32_t grid;
    uint32_t fill;
};

inline constexpr Palette kDefaultPalette = {0x3DDC84, 0x5F7A66, 0x13261B, 0x1E2B23, 0x0B110E,
                                            0x2E4A38, 0x16211B, 0x0C1611, 0x0D2216};

struct AccentPreset {
    const char *name;
    uint32_t color;
};

// Settings lists these in order; the stored config value is the index.
inline constexpr std::array<AccentPreset, 7> kAccentPresets = {{
    {"green", 0x3DDC84},
    {"orange", 0xFF6A1A},
    {"white", 0xF0F0F0},
    {"cyan", 0x5AD8E6},
    {"blue", 0x6E9BFF},
    {"violet", 0x9B8CFF},
    {"pink", 0xFF5FA2},
}};

inline constexpr const char *kAccentConfigKey = "accent";

// A stored index outside the list falls back to the default accent.
inline constexpr int accent_index(int stored)
{
    return stored >= 0 && stored < static_cast<int>(kAccentPresets.size()) ? stored : 0;
}

struct Hsl {
    double h;
    double s;
    double l;
};

inline Hsl to_hsl(uint32_t rgb)
{
    const double r = ((rgb >> 16) & 0xFF) / 255.0, g = ((rgb >> 8) & 0xFF) / 255.0, b = (rgb & 0xFF) / 255.0;
    const double hi = std::max({r, g, b}), lo = std::min({r, g, b}), delta = hi - lo;
    Hsl out{0.0, 0.0, (hi + lo) / 2.0};
    if (delta < 1e-9) return out;
    out.s = delta / (1.0 - std::fabs(2.0 * out.l - 1.0));
    if (hi == r) out.h = std::fmod((g - b) / delta + 6.0, 6.0);
    else if (hi == g) out.h = (b - r) / delta + 2.0;
    else out.h = (r - g) / delta + 4.0;
    out.h *= 60.0;
    return out;
}

inline uint32_t from_hsl(const Hsl &hsl)
{
    const double c = (1.0 - std::fabs(2.0 * hsl.l - 1.0)) * hsl.s;
    const double x = c * (1.0 - std::fabs(std::fmod(hsl.h / 60.0, 2.0) - 1.0));
    const double m = hsl.l - c / 2.0;
    double r = 0.0, g = 0.0, b = 0.0;
    switch (static_cast<int>(hsl.h / 60.0) % 6) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    const auto channel = [m](double v) {
        return static_cast<uint32_t>(std::clamp(std::lround((v + m) * 255.0), 0L, 255L));
    };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

// The neutral `base` with the accent's hue. A grey accent gives grey neutrals.
inline uint32_t tint(uint32_t base, uint32_t accent)
{
    const Hsl from = to_hsl(base), to = to_hsl(accent);
    const double reference = to_hsl(kDefaultPalette.accent).s;
    return from_hsl({to.h, from.s * std::min(1.0, to.s / reference), from.l});
}

inline Palette accent_palette(uint32_t accent)
{
    const Palette &d = kDefaultPalette;
    if (accent == d.accent) return d;
    return {accent,
            tint(d.dim, accent),
            tint(d.select, accent),
            tint(d.line, accent),
            tint(d.panel, accent),
            tint(d.bracket, accent),
            tint(d.track, accent),
            tint(d.grid, accent),
            tint(d.fill, accent)};
}

} // namespace cp0::theme
