/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_font_service.hpp"

namespace settings_fonts {

// Settings text uses the launcher UI font; the pixel font has one weight, so
// the style argument is kept for call-site compatibility only.
inline const lv_font_t *sans(uint16_t size,
                             lv_freetype_font_style_t style = LV_FREETYPE_FONT_STYLE_NORMAL)
{
    (void)style;
    return cp0_fonts().get_chrome(size);
}

inline const lv_font_t *mono(uint16_t size,
                             lv_freetype_font_style_t style = LV_FREETYPE_FONT_STYLE_NORMAL)
{
    (void)style;
    return cp0_fonts().get_chrome(size);
}

// User strings (SSIDs, device names) also use the pixel font, falling back to
// DejaVu Sans. No CJK font ships, so CJK glyphs do not render.
inline const lv_font_t *cjk_sans(uint16_t size,
                                 lv_freetype_font_style_t style = LV_FREETYPE_FONT_STYLE_NORMAL)
{
    (void)style;
    return cp0_fonts().get_chrome(size);
}

} // namespace settings_fonts
