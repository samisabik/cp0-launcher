/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_font_service.hpp"

namespace settings_fonts {

// Settings text, including user strings (SSIDs, device names), uses the
// launcher UI font, falling back to DejaVu Sans. No CJK font ships, so CJK
// glyphs do not render.
inline const lv_font_t *font(uint16_t size)
{
    return cp0_fonts().get_chrome(size);
}

} // namespace settings_fonts
