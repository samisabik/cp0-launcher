/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_font_service.hpp"

#include "cp0_font_cache_policy.hpp"
#include "cp0_sync_signal.hpp"
#include "hal_lvgl_bsp.h"

#include <utility>

Cp0FontService::~Cp0FontService()
{
    release();
}

lv_font_t *Cp0FontService::get(const char *font_name, uint16_t size,
                               lv_freetype_font_style_t style,
                               lv_freetype_font_render_mode_t render_mode)
{
#if LV_USE_FREETYPE
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string request_key = key(font_name ? font_name : "", size, style, render_mode);
    const auto requested = requests_.find(request_key);
    if (requested != requests_.end()) return requested->second ? requested->second : fallback(size);

    lv_font_t *font = nullptr;
    const std::string path = resolve_path(font_name);
    if (!path.empty()) {
        const std::string font_key = key(path, size, style, render_mode);
        const auto found = fonts_.find(font_key);
        if (found != fonts_.end()) {
            font = found->second;
        } else {
            font = lv_freetype_font_create(path.c_str(), render_mode, size, style);
            if (cp0::font::should_cache(font)) {
                try {
                    fonts_.emplace(font_key, font);
                } catch (...) {
                    lv_freetype_font_delete(font);
                    font = nullptr;
                }
            }
        }
    }
    try {
        requests_.emplace(request_key, font);
    } catch (...) {
    }
    return font ? font : fallback(size);
#else
    (void)font_name;
    (void)style;
    (void)render_mode;
    return fallback(size);
#endif
}

lv_font_t *Cp0FontService::get_mono(const char *font_name, uint16_t size,
                                    lv_freetype_font_style_t style)
{
    return get(font_name, size, style,
               static_cast<lv_freetype_font_render_mode_t>(2));
}

lv_font_t *Cp0FontService::get_chrome(uint16_t size)
{
    const int slot = size > 16 ? 1 : 0;
#if LV_USE_FREETYPE
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (chrome_[slot]) return chrome_[slot];
    }
#endif
    const uint16_t px = slot ? kCp0ChromeFontSize * 2 : kCp0ChromeFontSize;
    lv_font_t *font = get_mono(kCp0ChromeFont, px);
    if (font && !font->fallback && font != fallback(px)) {
        lv_font_t *latin = get("DejaVuSans.ttf", px);
        if (latin && latin != font && latin != fallback(px)) {
            if (!latin->fallback) {
                lv_font_t *cjk = get("AlibabaPuHuiTi-3-55-Regular.ttf", px);
                if (cjk && cjk != latin && cjk != fallback(px)) latin->fallback = cjk;
            }
            font->fallback = latin;
        }
    }
#if LV_USE_FREETYPE
    if (font && font != fallback(px)) {
        std::lock_guard<std::mutex> lock(mutex_);
        chrome_[slot] = font;
    }
#endif
    return font;
}

#if LV_USE_FREETYPE
std::string Cp0FontService::resolve_path(const char *font_name)
{
    if (!font_name || !font_name[0]) return {};
    std::string value(font_name);
    if (value.find('/') != std::string::npos ||
        (value.size() > 1 && value[1] == ':')) return value;

    std::string resolved;
    cp0::signal::invoke_noexcept([&] { cp0_signal_filesystem_api({"Path", value}, [&](int code, std::string data) {
        if (code == 0) resolved = std::move(data);
    }); });
    return resolved.empty() ? value : resolved;
}

std::string Cp0FontService::key(const std::string &path, uint16_t size,
                                lv_freetype_font_style_t style,
                                lv_freetype_font_render_mode_t render_mode)
{
    return path + "#" + std::to_string(size) + "#" +
           std::to_string(static_cast<int>(style)) + "#" +
           std::to_string(static_cast<int>(render_mode));
}
#endif

lv_font_t *Cp0FontService::fallback(uint16_t size) const
{
#if LV_FONT_MONTSERRAT_20
    if (size >= 18) return const_cast<lv_font_t *>(&lv_font_montserrat_20);
#endif
#if LV_FONT_MONTSERRAT_14
    if (size >= 14) return const_cast<lv_font_t *>(&lv_font_montserrat_14);
#endif
#if LV_FONT_MONTSERRAT_12
    if (size >= 12) return const_cast<lv_font_t *>(&lv_font_montserrat_12);
#endif
#if LV_FONT_MONTSERRAT_10
    (void)size;
    return const_cast<lv_font_t *>(&lv_font_montserrat_10);
#else
    (void)size;
    return const_cast<lv_font_t *>(LV_FONT_DEFAULT);
#endif
}

void Cp0FontService::release()
{
#if LV_USE_FREETYPE
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &font : fonts_) {
        if (font.second) lv_freetype_font_delete(font.second);
    }
    fonts_.clear();
    requests_.clear();
    chrome_[0] = chrome_[1] = nullptr;
#endif
}

Cp0FontService &cp0_fonts()
{
    static Cp0FontService service;
    return service;
}
