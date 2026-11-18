/*
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/slime_model.hpp"
#include "cp0_theme.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================
//  Slime  UISlimePage
//  Physarum (slime mould) agent simulation, full panel, CPU only.
//
//  Keys:
//    R              reseed, cycling the start pattern (scatter / disc / ring)
//    1..6           species (parameter presets)
//    UP / DOWN      wider / narrower sensor angle
//    LEFT / RIGHT   fewer / more agents
//    SPACE          pause
//    ESC            home
// ============================================================
class UISlimePage : public AppPage
{
public:
    UISlimePage();
    ~UISlimePage() override;

private:
    static constexpr int kFieldW = 310;
    static constexpr int kFieldH = 130;

    struct Command {
        enum class Type { Reseed, Species, SensorAngle, Agents } type;
        int value;
        uint32_t seed;
    };

    int species_ = 0;
    int agents_ = 3500;
    int sensor_angle_ = 0;
    int pattern_ = 0;
    uint32_t seed_ = 1;
    bool paused_ = false;
    std::string message_;
    uint32_t message_until_ = 0;

    // The worker owns model_ once it runs. It publishes one palette index per
    // cell in frame_, which the LVGL timer swaps out under mutex_.
    slime::Model model_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool worker_stop_ = false;
    bool worker_paused_ = false;
    std::vector<Command> commands_;
    std::vector<uint8_t> frame_;
    uint64_t frame_sequence_ = 0;
    uint64_t frame_drawn_ = 0;

    std::vector<uint8_t> levels_;
    std::array<uint16_t, 64> palette_{};
    std::vector<uint16_t> pixels_;

    lv_obj_t *status_ = nullptr;
    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    void create_ui();
    void show_message(const std::string &text);
    void push(Command::Type type, int value, uint32_t seed = 0);
    void worker_loop();
    void draw_frame();
    void render();
    void on_timer();
    void handle_key(uint32_t key, bool repeat);
    static void timer_cb(lv_timer_t *timer) noexcept;
    static void event_cb(lv_event_t *event) noexcept;
};
