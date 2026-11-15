/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl/lvgl.h"

#if LV_USE_FREETYPE
#include "lvgl/src/libs/freetype/lv_freetype.h"
#else
#ifndef LV_FREETYPE_FONT_STYLE_NORMAL
typedef int lv_freetype_font_style_t;
#define LV_FREETYPE_FONT_STYLE_NORMAL 0
#define LV_FREETYPE_FONT_STYLE_ITALIC (1 << 0)
#define LV_FREETYPE_FONT_STYLE_BOLD (1 << 1)
#endif
#ifndef LV_FREETYPE_FONT_RENDER_MODE_BITMAP
typedef int lv_freetype_font_render_mode_t;
#define LV_FREETYPE_FONT_RENDER_MODE_BITMAP 0
#define LV_FREETYPE_FONT_RENDER_MODE_OUTLINE 1
#endif
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Pixel font drawn on an 11 px grid; it only renders crisply at multiples of 11.
inline constexpr const char *kCp0ChromeFont = "DepartureMono-Regular.otf";
inline constexpr uint16_t kCp0ChromeFontSize = 11;

class Cp0FontService
{
public:
    Cp0FontService() = default;
    ~Cp0FontService();

    Cp0FontService(const Cp0FontService &) = delete;
    Cp0FontService &operator=(const Cp0FontService &) = delete;

    lv_font_t *get(const char *font_name, uint16_t size,
                   lv_freetype_font_style_t style = LV_FREETYPE_FONT_STYLE_NORMAL,
                   lv_freetype_font_render_mode_t render_mode = LV_FREETYPE_FONT_RENDER_MODE_BITMAP);
    lv_font_t *get_mono(const char *font_name, uint16_t size,
                        lv_freetype_font_style_t style = LV_FREETYPE_FONT_STYLE_NORMAL);
    // UI font: kCp0ChromeFont at the nearest crisp size (11 or 22 px), falling
    // back to DejaVu Sans and then the CJK font for glyphs it does not cover.
    lv_font_t *get_chrome(uint16_t size);

    lv_font_t *fallback(uint16_t size) const;
    void release();

private:
#if LV_USE_FREETYPE
    static std::string resolve_path(const char *font_name);
    static std::string key(const std::string &path, uint16_t size,
                           lv_freetype_font_style_t style,
                           lv_freetype_font_render_mode_t render_mode);
    // Faces by resolved path, and the result of each request by name so a
    // repeat lookup skips the path resolution. nullptr marks a failed load.
    std::unordered_map<std::string, lv_font_t *> fonts_;
    std::unordered_map<std::string, lv_font_t *> requests_;
    std::mutex mutex_;
#endif
    lv_font_t *chrome_[2] = {nullptr, nullptr};
};

Cp0FontService &cp0_fonts();
