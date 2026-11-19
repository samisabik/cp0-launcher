/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../../ext_components/cp0_lvgl/include/cp0_accent.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace cp0::theme;

namespace {

double hue_distance(double a, double b)
{
    const double d = std::fabs(a - b);
    return std::min(d, 360.0 - d);
}

void check_tint(uint32_t base, uint32_t derived, uint32_t accent)
{
    const Hsl from = to_hsl(base), to = to_hsl(derived), target = to_hsl(accent);
    assert(std::fabs(from.l - to.l) < 0.012);
    if (target.s < 0.05) {
        assert(to.s < 0.03);
        return;
    }
    // Rounding to 8 bits moves the hue of a near-black colour by many degrees,
    // so the hue is only checked where the channels are far enough apart.
    const int r = (derived >> 16) & 0xFF, g = (derived >> 8) & 0xFF, b = derived & 0xFF;
    if (std::max({r, g, b}) - std::min({r, g, b}) >= 24) assert(hue_distance(to.h, target.h) < 8.0);
    // The strongest channel of the accent is never the weakest of the tint.
    const int ar = (accent >> 16) & 0xFF, ag = (accent >> 8) & 0xFF, ab = accent & 0xFF;
    const int strongest = ar >= ag && ar >= ab ? r : ag >= ab ? g : b;
    assert(strongest >= std::max({r, g, b}) - 1);
}

} // namespace

int main()
{
    // The first preset is the look the launcher shipped with, unchanged.
    assert(kAccentPresets[0].color == kDefaultPalette.accent);
    const Palette same = accent_palette(kDefaultPalette.accent);
    assert(same.accent == 0x3DDC84 && same.dim == 0x5F7A66 && same.select == 0x13261B && same.line == 0x1E2B23);
    assert(same.panel == 0x0B110E && same.bracket == 0x2E4A38 && same.track == 0x16211B && same.grid == 0x0C1611);
    assert(same.fill == 0x0D2216);

    assert(accent_index(0) == 0 && accent_index(6) == 6);
    assert(accent_index(-1) == 0 && accent_index(7) == 0 && accent_index(1000) == 0);

    // Every other preset keeps each neutral's lightness and takes the accent's hue.
    const Palette &d = kDefaultPalette;
    for (const AccentPreset &preset : kAccentPresets) {
        const Palette p = accent_palette(preset.color);
        assert(p.accent == preset.color);
        // The default palette is hand-picked, not derived.
        if (preset.color == d.accent) continue;
        check_tint(d.dim, p.dim, preset.color);
        check_tint(d.select, p.select, preset.color);
        check_tint(d.line, p.line, preset.color);
        check_tint(d.panel, p.panel, preset.color);
        check_tint(d.bracket, p.bracket, preset.color);
        check_tint(d.track, p.track, preset.color);
        check_tint(d.grid, p.grid, preset.color);
        check_tint(d.fill, p.fill, preset.color);
    }

    // A grey accent gives grey neutrals: no leftover green.
    const Palette white = accent_palette(0xF0F0F0);
    for (uint32_t c : {white.dim, white.select, white.line, white.panel, white.bracket, white.track, white.grid}) {
        const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        assert(std::abs(r - g) <= 1 && std::abs(g - b) <= 1);
    }

    // Round trip through HSL is exact enough to reproduce a colour.
    for (uint32_t c : {0x3DDC84u, 0xFF6A1Au, 0x5F7A66u, 0x000000u, 0xFFFFFFu, 0x6E9BFFu})
        assert(from_hsl(to_hsl(c)) == c);

    std::puts("test_accent: ok");
    return 0;
}
