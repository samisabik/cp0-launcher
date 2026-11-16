/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "cp0_font_service.hpp"
#include "cp0_lvgl_app.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// Shared look for the launcher, Settings and built-in apps on the 320x170
// full-bleed screen: one palette, the DepartureMono chrome font at 11/22 px,
// and a title row / body / footer frame.
namespace cp0::theme {

inline constexpr uint32_t kBg = 0x000000;
inline constexpr uint32_t kAccent = 0x3DDC84;
inline constexpr uint32_t kText = 0xE6E6E6;
inline constexpr uint32_t kIdle = 0x8A8A8A;
inline constexpr uint32_t kDim = 0x5F7A66;
inline constexpr uint32_t kSelect = 0x13261B;
inline constexpr uint32_t kLine = 0x1E2B23;
inline constexpr uint32_t kWarn = 0xE0A84E;
inline constexpr uint32_t kDanger = 0xFF5F56;
// Panels: dark filled boxes with corner brackets over a faint dot grid.
inline constexpr uint32_t kPanel = 0x0B110E;
inline constexpr uint32_t kBracket = 0x2E4A38;
inline constexpr uint32_t kTrack = 0x16211B;
inline constexpr uint32_t kGrid = 0x0C1611;

inline constexpr uint16_t kFontPx = 11;
inline constexpr uint16_t kFontLargePx = 22;

inline constexpr int kScreenW = 320;
inline constexpr int kScreenH = 170;
inline constexpr int kPadX = 4;
inline constexpr int kTitleY = 1;
inline constexpr int kTitleRuleY = 15;
inline constexpr int kBodyTop = 17;
inline constexpr int kFooterRuleY = 153;
inline constexpr int kFooterY = 156;
inline constexpr int kBodyH = kFooterRuleY - kBodyTop;

} // namespace cp0::theme

namespace cp0::ui {

inline const lv_font_t *font(bool large = false)
{
    return cp0_fonts().get_chrome(large ? theme::kFontLargePx : theme::kFontPx);
}

// Style setters invalidate the object even when the value is unchanged, so
// pages that refresh on a timer must only write styles that actually change.
inline void set_text_color(lv_obj_t *obj, uint32_t color)
{
    if (!obj) return;
    const lv_color_t c = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), c)) lv_obj_set_style_text_color(obj, c, 0);
}

inline void set_bg_color(lv_obj_t *obj, uint32_t color)
{
    if (!obj) return;
    const lv_color_t c = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), c)) lv_obj_set_style_bg_color(obj, c, 0);
}

inline lv_obj_t *label(lv_obj_t *parent, int x, int y, uint32_t color, bool large = false)
{
    lv_obj_t *obj = parent ? lv_label_create(parent) : nullptr;
    if (!obj) return nullptr;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font(large), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, "");
    return obj;
}

inline lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = parent ? lv_obj_create(parent) : nullptr;
    if (!obj) return nullptr;
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

inline void paint_background(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(theme::kBg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

// Tiled one-pixel patterns: a dot every 6 px for the grid, every 2 px for rules,
// and a 1 px gap every 3 px that cuts a bar into segments.
inline const lv_image_dsc_t *pattern(int w, int h, uint32_t rgb, bool invert = false)
{
    struct Slot { int w, h; uint32_t rgb; bool invert; std::array<uint32_t, 36> px; lv_image_dsc_t dsc; };
    static std::array<Slot, 6> slots{};
    static int used = 0;
    for (int i = 0; i < used; ++i)
        if (slots[i].w == w && slots[i].h == h && slots[i].rgb == rgb && slots[i].invert == invert) return &slots[i].dsc;
    if (used >= static_cast<int>(slots.size()) || w * h > 36) return nullptr;
    Slot &slot = slots[used++];
    slot.w = w; slot.h = h; slot.rgb = rgb; slot.invert = invert;
    const uint32_t on = 0xFF000000u | rgb;
    for (int i = 0; i < w * h; ++i) slot.px[i] = (i == 0) != invert ? on : 0;
    slot.dsc = {};
    slot.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    slot.dsc.header.w = static_cast<uint32_t>(w);
    slot.dsc.header.h = static_cast<uint32_t>(h);
    slot.dsc.header.stride = static_cast<uint32_t>(w * 4);
    slot.dsc.data_size = static_cast<uint32_t>(w * h * 4);
    slot.dsc.data = reinterpret_cast<const uint8_t *>(slot.px.data());
    return &slot.dsc;
}

inline lv_obj_t *tiled(lv_obj_t *parent, int x, int y, int w, int h, const lv_image_dsc_t *image)
{
    lv_obj_t *obj = box(parent, x, y, w, h, theme::kBg);
    if (!obj) return nullptr;
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    if (image) {
        lv_obj_set_style_bg_image_src(obj, image, 0);
        lv_obj_set_style_bg_image_tiled(obj, true, 0);
    }
    return obj;
}

inline lv_obj_t *dot_grid(lv_obj_t *parent, int x, int y, int w, int h)
{
    return tiled(parent, x, y, w, h, pattern(6, 6, theme::kGrid));
}

inline lv_obj_t *dotted_rule(lv_obj_t *parent, int x, int y, int w, uint32_t color = theme::kLine)
{
    return tiled(parent, x, y, w, 1, pattern(2, 1, color));
}

// Corner brackets: four 1 px L shapes inside an object's corners.
struct Brackets {
    std::array<lv_obj_t *, 4> corner{};
};

inline Brackets brackets(lv_obj_t *parent, uint32_t color, int len = 4)
{
    Brackets out;
    static constexpr lv_align_t kAlign[4] = {LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT,
                                             LV_ALIGN_BOTTOM_RIGHT};
    static constexpr lv_border_side_t kSide[4] = {
        static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_TOP),
        static_cast<lv_border_side_t>(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_TOP),
        static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM),
        static_cast<lv_border_side_t>(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM)};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *c = box(parent, 0, 0, len, len, theme::kBg);
        if (!c) continue;
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_align(c, kAlign[i]);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_side(c, kSide[i], 0);
        lv_obj_set_style_border_color(c, lv_color_hex(color), 0);
        out.corner[i] = c;
    }
    return out;
}

inline void set_brackets(const Brackets &b, uint32_t color)
{
    const lv_color_t value = lv_color_hex(color);
    for (lv_obj_t *c : b.corner)
        if (c && !lv_color_eq(lv_obj_get_style_border_color(c, LV_PART_MAIN), value))
            lv_obj_set_style_border_color(c, value, 0);
}

// Panel: a dark filled box with corner brackets. Content goes inside `box`,
// positioned relative to it. Focus swaps the fill to kSelect and the brackets to accent.
struct Panel {
    lv_obj_t *box = nullptr;
    Brackets corners;
};

inline Panel panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    Panel p;
    p.box = box(parent, x, y, w, h, theme::kPanel);
    if (p.box) p.corners = brackets(p.box, theme::kBracket);
    return p;
}

inline void set_panel_focus(const Panel &p, bool focus, uint32_t edge = theme::kAccent)
{
    if (!p.box) return;
    set_bg_color(p.box, focus ? theme::kSelect : theme::kPanel);
    set_brackets(p.corners, focus ? edge : theme::kBracket);
}

// Readout: a panel with a small dim label over a 22 px value and an idle unit.
struct Readout {
    Panel panel;
    lv_obj_t *label = nullptr;
    lv_obj_t *value = nullptr;
    lv_obj_t *unit = nullptr;
};

inline Readout readout(lv_obj_t *parent, int x, int y, int w, int h, const char *caption)
{
    Readout r;
    r.panel = panel(parent, x, y, w, h);
    if (!r.panel.box) return r;
    r.label = label(r.panel.box, 5, 2, theme::kDim);
    if (r.label) lv_label_set_text(r.label, caption ? caption : "");
    lv_obj_t *row = box(r.panel.box, 5, 13, w - 10, 26, theme::kBg);
    if (!row) return r;
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 4, 0);
    r.value = label(row, 0, 0, theme::kText, true);
    r.unit = label(row, 0, 0, theme::kIdle);
    if (r.unit) lv_obj_set_style_pad_bottom(r.unit, 3, 0);
    return r;
}

inline void set_readout(const Readout &r, const std::string &value, const std::string &unit = {},
                        uint32_t color = theme::kText)
{
    if (r.value) {
        if (value != lv_label_get_text(r.value)) lv_label_set_text(r.value, value.c_str());
        set_text_color(r.value, color);
    }
    if (r.unit && unit != lv_label_get_text(r.unit)) lv_label_set_text(r.unit, unit.c_str());
}

// Segmented bar: a track with a fill, cut into 2 px segments by a tiled overlay.
struct SegBar {
    lv_obj_t *track = nullptr;
    lv_obj_t *fill = nullptr;
    int width = 0;
};

inline SegBar segbar(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg = theme::kPanel)
{
    SegBar b;
    b.width = w;
    b.track = box(parent, x, y, w, h, theme::kTrack);
    if (!b.track) return b;
    b.fill = box(b.track, 0, 0, 0, h, theme::kIdle);
    tiled(b.track, 0, 0, w, h, pattern(3, 1, bg, true));
    return b;
}

inline void set_segbar(const SegBar &b, float fraction, uint32_t color)
{
    if (!b.fill) return;
    const float f = fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
    const int w = static_cast<int>(f * static_cast<float>(b.width) + 0.5f);
    if (lv_obj_get_width(b.fill) != w) lv_obj_set_width(b.fill, w);
    const lv_color_t c = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_bg_color(b.fill, LV_PART_MAIN), c)) lv_obj_set_style_bg_color(b.fill, c, 0);
}

// Battery percentage and four Wi-Fi bars at the right of the title row,
// refreshed every few seconds and torn down with their parent.
struct StatusCluster {
    lv_obj_t *battery = nullptr;
    std::array<lv_obj_t *, 4> bars{};
    lv_timer_t *timer = nullptr;
};

inline void refresh_status(StatusCluster *s)
{
    if (!s) return;
    const cp0_battery_info_t battery = cp0_battery_read();
    if (s->battery) {
        char text[8] = "";
        if (battery.valid) std::snprintf(text, sizeof(text), "%d%%", battery.soc);
        if (std::string(lv_label_get_text(s->battery)) != text) lv_label_set_text(s->battery, text);
        set_text_color(s->battery, (battery.flags & 1) ? theme::kAccent : theme::kIdle);
    }
    cp0_wifi_status_t wifi{};
    if (cp0_wifi_status_read(&wifi) != 0) wifi = {};
    int strength = wifi.signal < 0 ? 2 * (wifi.signal + 100) : wifi.signal;
    static constexpr int kThresholds[4] = {1, 30, 60, 80};
    for (int i = 0; i < 4; ++i) {
        if (!s->bars[i]) continue;
        const bool lit = wifi.connected && strength >= kThresholds[i];
        set_bg_color(s->bars[i], lit ? theme::kAccent : theme::kDim);
        if (wifi.connected) lv_obj_remove_flag(s->bars[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s->bars[i], LV_OBJ_FLAG_HIDDEN);
    }
}

inline StatusCluster *status_cluster(lv_obj_t *parent)
{
    if (!parent) return nullptr;
    auto *s = new StatusCluster();
    static constexpr int kHeights[4] = {3, 5, 7, 9};
    for (int i = 0; i < 4; ++i)
        s->bars[i] = box(parent, theme::kScreenW - theme::kPadX - 15 + i * 4, 12 - kHeights[i], 3, kHeights[i],
                         theme::kDim);
    s->battery = label(parent, theme::kScreenW - theme::kPadX - 19 - 40, theme::kTitleY, theme::kIdle);
    if (s->battery) {
        lv_obj_set_width(s->battery, 40);
        lv_obj_set_style_text_align(s->battery, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_event_cb(
            s->battery,
            [](lv_event_t *e) {
                auto *cluster = static_cast<StatusCluster *>(lv_event_get_user_data(e));
                if (cluster->timer) lv_timer_delete(cluster->timer);
                delete cluster;
            },
            LV_EVENT_DELETE, s);
        s->timer = lv_timer_create([](lv_timer_t *t) { refresh_status(static_cast<StatusCluster *>(lv_timer_get_user_data(t))); },
                                   5000, s);
    }
    refresh_status(s);
    if (!s->battery) {
        delete s;
        return nullptr;
    }
    return s;
}

// Title row: title on the left, an idle status before the battery and Wi-Fi
// cluster on the right, and a dotted rule under both.
struct TitleRow {
    lv_obj_t *title = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *rule = nullptr;
};

inline constexpr int kStatusRight = theme::kScreenW - theme::kPadX - 19 - 44;

inline TitleRow title_row(lv_obj_t *parent, const char *title)
{
    TitleRow row;
    row.title = label(parent, theme::kPadX, theme::kTitleY, theme::kText);
    if (row.title) {
        lv_obj_set_width(row.title, kStatusRight - theme::kPadX);
        lv_label_set_long_mode(row.title, LV_LABEL_LONG_CLIP);
        lv_label_set_text(row.title, title ? title : "");
    }
    row.status = label(parent, kStatusRight - 140, theme::kTitleY, theme::kIdle);
    if (row.status) {
        lv_obj_set_width(row.status, 140);
        lv_obj_set_style_text_align(row.status, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(row.status, LV_LABEL_LONG_CLIP);
    }
    status_cluster(parent);
    row.rule = dotted_rule(parent, 0, theme::kTitleRuleY, theme::kScreenW);
    return row;
}

// UI chrome is lowercase; use for status strings that come from models.
inline std::string lower(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

inline void set_text(lv_obj_t *obj, const char *text, uint32_t color)
{
    if (!obj) return;
    const char *next = text ? text : "";
    if (std::string(lv_label_get_text(obj)) != next) lv_label_set_text(obj, next);
    set_text_color(obj, color);
}

// Footer: key/action chips on one line, key in accent and action in idle grey.
// Chips that do not fit are dropped from the end instead of being clipped.
using Chip = std::pair<const char *, const char *>;

inline lv_obj_t *footer(lv_obj_t *parent)
{
    dotted_rule(parent, 0, theme::kFooterRuleY, theme::kScreenW);
    lv_obj_t *obj = label(parent, theme::kPadX, theme::kFooterY, theme::kDim);
    if (!obj) return nullptr;
    lv_obj_set_width(obj, theme::kScreenW - 2 * theme::kPadX);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_label_set_recolor(obj, true);
    return obj;
}

inline int footer_columns()
{
    const lv_font_t *f = font();
    const int advance = f ? lv_font_get_glyph_width(f, '0', 0) : 7;
    return (theme::kScreenW - 2 * theme::kPadX) / (advance > 0 ? advance : 7);
}

inline void set_footer(lv_obj_t *obj, const std::vector<Chip> &chips)
{
    if (!obj) return;
    char color[16];
    std::string text;
    int used = 0;
    const int columns = footer_columns();
    for (const Chip &chip : chips) {
        const std::string key = chip.first ? chip.first : "";
        const std::string word = chip.second ? chip.second : "";
        const int width = static_cast<int>(key.size() + (word.empty() ? 0 : word.size() + 1)) + (used ? 2 : 0);
        if (used + width > columns) break;
        if (used) text += "  ";
        std::snprintf(color, sizeof(color), "#%06X ", static_cast<unsigned>(theme::kAccent));
        text += color + key + "#";
        if (!word.empty()) {
            std::snprintf(color, sizeof(color), " #%06X ", static_cast<unsigned>(theme::kIdle));
            text += color + word + "#";
        }
        used += width;
    }
    set_text_color(obj, theme::kDim);
    if (std::string(lv_label_get_text(obj)) != text) lv_label_set_text(obj, text.c_str());
}

// A transient line in the footer slot, e.g. "sent 433.92" or an error.
inline void set_footer_message(lv_obj_t *obj, const char *text, uint32_t color = theme::kAccent)
{
    set_text(obj, text, color);
}

} // namespace cp0::ui
