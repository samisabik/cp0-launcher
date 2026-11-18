/*
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_slime.hpp"

#include "../app_support.hpp"
#include "cp0_lvgl_app.h"
#include "cp0_theme.hpp"
#include "input_keys.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr uint32_t kRefreshMs = 66;
constexpr uint32_t kMessageMs = 2000;
constexpr int kStepsPerFrame = 2;

constexpr const char *kSpeciesConfigKey = "slime_species";
constexpr const char *kAgentsConfigKey = "slime_agents";
constexpr const char *kPaletteConfigKey = "slime_palette";

int config_int(const char *key, int fallback)
{
    int value = fallback;
    cp0_signal_config_api({"GetInt", key, std::to_string(fallback)}, [&](int code, std::string data) {
        if (code == 0) value = std::atoi(data.c_str());
    });
    return value;
}

void set_config_int(const char *key, int value)
{
    cp0_signal_config_api({"SetInt", key, std::to_string(value)}, nullptr);
    cp0_signal_config_api({"Save"}, nullptr);
}

} // namespace

UISlimePage::UISlimePage() : AppPage(), model_(kFieldW, kFieldH, 0, 1u)
{
    set_page_title("Slime");
    reclaim_top_bar();
    species_ = std::clamp(config_int(kSpeciesConfigKey, 0), 0, static_cast<int>(slime::kSpecies.size()) - 1);
    agents_ = slime::clamp_agents(config_int(kAgentsConfigKey, agents_));
    sensor_angle_ = slime::kSpecies[species_].params.sensor_angle_deg;
    seed_ = lv_tick_get() * 2654435761u + 1u;

    colour_ = std::clamp(config_int(kPaletteConfigKey, 1), 0, slime::kPaletteCount - 1);
    load_palette();
    const std::size_t cells = static_cast<std::size_t>(kFieldW) * kFieldH;
    pixels_.assign(cells, palette_[0]);
    levels_.assign(cells, 0);
    frame_.assign(cells, 0);

    model_.set_params(slime::kSpecies[species_].params);
    model_.set_agent_count(agents_);
    model_.reseed(seed_, slime::Pattern::Scatter);

    create_ui();
    render();
    worker_ = std::thread([this] { worker_loop(); });
    timer_ = lv_timer_create(timer_cb, kRefreshMs, this);
    if (root_screen_)
        lv_obj_add_event_cb(root_screen_, event_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
}

UISlimePage::~UISlimePage()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    if (root_screen_) lv_obj_remove_event_cb_with_user_data(root_screen_, event_cb, this);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void UISlimePage::create_ui()
{
    if (!ui_APP_Container) return;
    cp0::ui::paint_background(ui_APP_Container);
    const cp0::ui::TitleRow row = cp0::ui::title_row(ui_APP_Container, "slime");
    status_ = row.status;
    cp0::ui::panel(ui_APP_Container, 4, 19, 312, 132);
    canvas_ = lv_canvas_create(ui_APP_Container);
    if (canvas_) {
        lv_canvas_set_buffer(canvas_, pixels_.data(), kFieldW, kFieldH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 5, 20);
    }
    hint_ = cp0::ui::footer(ui_APP_Container);
    cp0::ui::set_footer(hint_, {{"r", "seed"}, {"1-6", "species"}, {"p", "colour"}, {"< >", "agents"}});
}

// Rebuilds the level-to-colour table and repaints the last frame with it, so
// a palette change shows at once even while paused.
void UISlimePage::load_palette()
{
    palette_[0] = app_support::rgb565(cp0::theme::kPanel);
    for (int i = 1; i < 64; ++i) palette_[i] = app_support::rgb565(slime::palette_rgb(colour_, i));
    for (std::size_t i = 0; i < pixels_.size() && i < levels_.size(); ++i) pixels_[i] = palette_[levels_[i] & 63];
    if (canvas_) lv_obj_invalidate(canvas_);
}

void UISlimePage::show_message(const std::string &text)
{
    message_ = text;
    message_until_ = lv_tick_get() + kMessageMs;
    if (message_until_ == 0) message_until_ = 1;
    render();
}

void UISlimePage::push(Command::Type type, int value, uint32_t seed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back({type, value, seed});
}

void UISlimePage::worker_loop()
{
    std::vector<uint8_t> levels(static_cast<std::size_t>(kFieldW) * kFieldH);
    std::vector<Command> commands;
    auto next = std::chrono::steady_clock::now();
    while (true) {
        bool paused = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_until(lock, next, [this] { return worker_stop_; });
            if (worker_stop_) break;
            commands.swap(commands_);
            paused = worker_paused_;
        }
        for (const Command &command : commands) {
            switch (command.type) {
            case Command::Type::Reseed:
                model_.reseed(command.seed, static_cast<slime::Pattern>(command.value));
                break;
            case Command::Type::Species:
                model_.set_params(slime::kSpecies[static_cast<std::size_t>(command.value)].params);
                break;
            case Command::Type::SensorAngle:
                model_.set_sensor_angle(command.value);
                break;
            case Command::Type::Agents:
                model_.set_agent_count(command.value);
                break;
            }
        }
        const bool changed = !commands.empty();
        commands.clear();
        if (!paused)
            for (int i = 0; i < kStepsPerFrame; ++i) model_.step();
        if (!paused || changed) {
            model_.levels(levels.data());
            std::lock_guard<std::mutex> lock(mutex_);
            frame_.swap(levels);
            ++frame_sequence_;
        }
        // A slow step drops frames instead of queueing them.
        next += std::chrono::milliseconds(kRefreshMs);
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;
    }
}

void UISlimePage::draw_frame()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame_sequence_ == frame_drawn_) return;
        frame_drawn_ = frame_sequence_;
        // The worker gets the previous buffer back; both stay the same size.
        frame_.swap(levels_);
    }
    if (!canvas_) return;
    const std::size_t cells = pixels_.size();
    for (std::size_t i = 0; i < cells; ++i) pixels_[i] = palette_[levels_[i] & 63];
    lv_obj_invalidate(canvas_);
}

void UISlimePage::render()
{
    if (!status_) return;
    if (message_until_ != 0) {
        cp0::ui::set_text(status_, message_.c_str(), cp0::theme::kText);
        return;
    }
    char text[40];
    std::snprintf(text, sizeof(text), "%s  %d", paused_ ? "paused" : slime::kSpecies[species_].name, agents_);
    cp0::ui::set_text(status_, text, paused_ ? cp0::theme::kIdle : cp0::theme::kAccent);
}

void UISlimePage::timer_cb(lv_timer_t *timer) noexcept
{
    auto *page = static_cast<UISlimePage *>(lv_timer_get_user_data(timer));
    if (!page) return;
    try {
        page->on_timer();
    } catch (...) {
    }
}

void UISlimePage::on_timer()
{
    draw_frame();
    if (message_until_ != 0 && static_cast<int32_t>(lv_tick_get() - message_until_) >= 0) {
        message_until_ = 0;
        render();
    }
}

void UISlimePage::event_cb(lv_event_t *event) noexcept
{
    auto *page = static_cast<UISlimePage *>(lv_event_get_user_data(event));
    if (!page || !launcher_ui::events::is_key_pressed(event)) return;
    const struct key_item *item = launcher_ui::events::keyboard_item(event);
    if (!item) return;
    const uint32_t key = item->semantic_key ? item->semantic_key : item->key_code;
    try {
        page->handle_key(key, item->key_state == KBD_KEY_REPEATED);
    } catch (...) {
    }
}

void UISlimePage::handle_key(uint32_t key, bool repeat)
{
    const bool arrow = key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN;
    if (repeat && !arrow) return;
    if (key == KEY_ESC) {
        if (navigate_home) navigate_home();
        return;
    }
    if (key == KEY_R) {
        pattern_ = (pattern_ + 1) % slime::kPatternCount;
        seed_ = seed_ * 1664525u + 1013904223u;
        push(Command::Type::Reseed, pattern_, seed_);
        show_message(slime::pattern_name(static_cast<slime::Pattern>(pattern_)));
        return;
    }
    if (key >= KEY_1 && key <= KEY_6) {
        species_ = static_cast<int>(key - KEY_1);
        sensor_angle_ = slime::kSpecies[species_].params.sensor_angle_deg;
        push(Command::Type::Species, species_);
        set_config_int(kSpeciesConfigKey, species_);
        message_until_ = 0;
        render();
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        sensor_angle_ =
            slime::clamp_sensor_angle(sensor_angle_ + (key == KEY_UP ? 1 : -1) * slime::kSensorAngleStep);
        push(Command::Type::SensorAngle, sensor_angle_);
        show_message("sense " + std::to_string(sensor_angle_) + " deg");
        return;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        const int next = slime::clamp_agents(agents_ + (key == KEY_RIGHT ? 1 : -1) * slime::kAgentStep);
        if (next == agents_) return;
        agents_ = next;
        push(Command::Type::Agents, agents_);
        set_config_int(kAgentsConfigKey, agents_);
        message_until_ = 0;
        render();
        return;
    }
    if (key == KEY_P) {
        colour_ = (colour_ + 1) % slime::kPaletteCount;
        load_palette();
        set_config_int(kPaletteConfigKey, colour_);
        show_message(slime::palette_name(colour_));
        return;
    }
    if (key == KEY_SPACE) {
        paused_ = !paused_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_paused_ = paused_;
        }
        message_until_ = 0;
        render();
    }
}
