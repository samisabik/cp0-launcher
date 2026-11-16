/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_hardware_page.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "hal_lvgl_bsp.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace {

enum class Badge { Ok, Missing, On, Off };

struct Probe {
    const char *name;
    Badge badge;
    std::string detail;
};

std::string first_line(const std::filesystem::path &path)
{
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

bool exists(const char *path)
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

Probe probe_cc1101()
{
    const bool found = exists("/dev/spidev0.1");
    return {"cc1101", found ? Badge::Ok : Badge::Missing, found ? "spidev0.1" : "no spidev0.1"};
}

Probe probe_rtl_sdr()
{
    std::error_code error;
    for (std::filesystem::directory_iterator it("/sys/bus/usb/devices", error), end; !error && it != end;
         it.increment(error)) {
        if (first_line(it->path() / "idVendor") == "0bda" && first_line(it->path() / "idProduct") == "2838")
            return {"rtl-sdr", Badge::Ok, "usb 0bda:2838"};
    }
    return {"rtl-sdr", Badge::Missing, "not on usb"};
}

Probe probe_usb_lan()
{
    if (!exists("/sys/class/net/eth0")) return {"usb lan", Badge::Missing, "no eth0"};
    const std::string state = first_line("/sys/class/net/eth0/operstate");
    return {"usb lan", Badge::Ok, state == "up" ? "eth0 link up" : "eth0 no link"};
}

Probe probe_mic()
{
    std::error_code error;
    for (std::filesystem::directory_iterator it("/dev/snd", error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        const std::size_t device = name.find('D', 4);
        if (name.rfind("pcmC", 0) == 0 && name.back() == 'c' && device != std::string::npos)
            return {"mic", Badge::Ok, "card " + name.substr(4, device - 4) + " capture"};
    }
    return {"mic", Badge::Missing, "no capture device"};
}

Probe probe_battery_gauge()
{
    const cp0_battery_info_t battery = cp0_battery_read();
    if (!battery.valid) return {"battery gauge", Badge::Missing, "no reading"};
    return {"battery gauge", Badge::Ok, "charge " + std::to_string(battery.soc) + "%"};
}

// Same GPIO read the ext port toggles use.
Probe probe_rail(const char *name, const char *port)
{
    std::string state;
    try {
        cp0_signal_settings_api({"GpioGet", port}, [&state](int code, std::string response) {
            if (code == 0 && (response == "0" || response == "1")) state = response;
        });
    } catch (...) {
    }
    if (state.empty()) return {name, Badge::Missing, "unreadable"};
    return {name, state == "1" ? Badge::On : Badge::Off, state == "1" ? "output on" : "output off"};
}

class LvSettingHardwarePage3 : public DComponens::LvglComponensBase {
public:
    LvSettingHardwarePage3(lv_obj_t *parent, std::function<void()> back_callback)
    {
        LeaveSelfPage = std::move(back_callback);
        create_ui(parent);
    }

    ~LvSettingHardwarePage3() override
    {
        if (ComponensObj) {
            lv_obj_delete(ComponensObj);
            ComponensObj = nullptr;
        }
    }

    void LoadNextPage() override {}

    void LeaveNextPage() override
    {
        if (LeaveSelfPage) LeaveSelfPage();
    }

    void create_ui(lv_obj_t *parent) override
    {
        if (!parent) return;
        ComponensObj = cp0::ui::box(parent, 0, 0, cp0::theme::kScreenW, cp0::theme::kScreenH, cp0::theme::kBg);
        if (!ComponensObj) return;
        DComponens::lvgl_bind_event(ComponensObj, LV_EVENT_KEY, nullptr,
                                    [this](lv_event_t *event) { handle_key_event(event); });
        cp0::ui::title_row(ComponensObj, "settings/system/hardware");

        const int panel_w = cp0::theme::kScreenW - 2 * cp0::theme::kPadX;
        const cp0::ui::Panel panel = cp0::ui::panel(ComponensObj, cp0::theme::kPadX, 19, panel_w, 131);
        for (std::size_t index = 0; index < rows_.size(); ++index) {
            const int y = 4 + static_cast<int>(index) * 18;
            Row &row = rows_[index];
            row.name = cp0::ui::label(panel.box, 8, y + 2, cp0::theme::kText);
            row.detail = cp0::ui::label(panel.box, 112, y + 2, cp0::theme::kIdle);
            if (row.detail) {
                lv_obj_set_width(row.detail, panel_w - 112 - 44);
                lv_label_set_long_mode(row.detail, LV_LABEL_LONG_CLIP);
            }
            row.badge = cp0::ui::box(panel.box, panel_w - 38, y + 1, 30, 15, cp0::theme::kPanel);
            if (row.badge) {
                lv_obj_set_style_border_width(row.badge, 1, 0);
                row.badge_text = cp0::ui::label(row.badge, 0, 1, cp0::theme::kDim);
                if (row.badge_text) {
                    lv_obj_set_width(row.badge_text, 28);
                    lv_obj_set_style_text_align(row.badge_text, LV_TEXT_ALIGN_CENTER, 0);
                }
            }
            if (index + 1 < rows_.size())
                cp0::ui::dotted_rule(panel.box, 6, y + 17, panel_w - 12);
        }
        cp0::ui::set_footer(cp0::ui::footer(ComponensObj), {{"ent", "refresh"}, {"esc", "back"}});
        refresh();
    }

private:
    struct Row {
        lv_obj_t *name = nullptr;
        lv_obj_t *detail = nullptr;
        lv_obj_t *badge = nullptr;
        lv_obj_t *badge_text = nullptr;
    };

    void refresh()
    {
        const std::array<Probe, 7> probes = {
            probe_cc1101(),        probe_rtl_sdr(),
            probe_usb_lan(),       probe_mic(),
            probe_battery_gauge(), probe_rail("ext 5v", "EXT5V"),
            probe_rail("grove 5v", "GROVE5V"),
        };
        for (std::size_t index = 0; index < rows_.size(); ++index) {
            const Probe &probe = probes[index];
            Row &row = rows_[index];
            const bool lit = probe.badge == Badge::Ok || probe.badge == Badge::On;
            const char *badge = probe.badge == Badge::Ok    ? "ok"
                                : probe.badge == Badge::On  ? "on"
                                : probe.badge == Badge::Off ? "off"
                                                            : "--";
            cp0::ui::set_text(row.name, probe.name, cp0::theme::kText);
            cp0::ui::set_text(row.detail, probe.detail.c_str(), lit ? cp0::theme::kIdle : cp0::theme::kDim);
            cp0::ui::set_text(row.badge_text, badge, lit ? cp0::theme::kAccent : cp0::theme::kDim);
            if (row.badge)
                lv_obj_set_style_border_color(row.badge, lv_color_hex(lit ? cp0::theme::kAccent : cp0::theme::kLine),
                                              0);
        }
    }

    void handle_key_event(lv_event_t *event)
    {
        if (!event || lv_event_get_code(event) != LV_EVENT_KEY) return;
        const uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_ESC || key == LV_KEY_LEFT) {
            if (LeaveSelfPage) LeaveSelfPage();
        } else if (key == LV_KEY_ENTER || key == LV_KEY_RIGHT) {
            refresh();
        }
        lv_event_stop_processing(event);
    }

    std::array<Row, 7> rows_{};
};

} // namespace

std::unique_ptr<DComponens::LvglComponensBase> settings_hardware_page_factory(lv_obj_t *parent,
                                                                             const NodeIter &,
                                                                             std::function<void()> back_callback)
{
    return std::make_unique<LvSettingHardwarePage3>(parent, std::move(back_callback));
}
