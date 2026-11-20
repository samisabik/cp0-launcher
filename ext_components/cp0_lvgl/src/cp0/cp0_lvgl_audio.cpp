/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "hal_lvgl_bsp.h"
#include "cp0_lvgl.h"
#include "../cp0_audio_api_contract.hpp"
#include "cp0_audio_mixer_control.hpp"
#include "../cp0_signal_registration.hpp"
#include "../cp0_audio_runtime_lifecycle.hpp"

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>


class AudioSystem
{
public:
    typedef std::function<void(int, std::string)> callback_t;

    void api_call(const std::list<std::string> &arg, callback_t callback)
    {
        cp0::audio::ApiRequest request;
        if(!cp0::audio::parse_api_request(arg, request)) {
            cp0::audio::invoke_callback(callback, -1, cp0::audio::invalid_api_request_message());
            return;
        }
        switch (request.command) {
        case cp0::audio::ApiCommand::VolumeRead:
            report_value(callback, mixer_.read_volume());
            break;
        case cp0::audio::ApiCommand::VolumeWrite:
            report_value(callback, mixer_.write_volume(request.value));
            break;
        case cp0::audio::ApiCommand::DefaultSink: {
            const std::string sink = mixer_.default_sink_name();
            cp0::audio::invoke_callback(callback, sink.empty() ? -1 : 0, sink);
            break;
        }
        case cp0::audio::ApiCommand::MuteRead:
            report_value(callback, mixer_.read_mute());
            break;
        case cp0::audio::ApiCommand::MuteToggle:
            report_value(callback, mixer_.toggle_mute());
            break;
        }
    }

private:
    static void report_value(const callback_t &callback, int value)
    {
        cp0::audio::invoke_callback(callback, value >= 0 ? 0 : -1, std::to_string(value));
    }

    Cp0AudioMixerControl mixer_;
};

namespace {

using AudioRegistrations = cp0::SignalRegistrationBundle<decltype(cp0_signal_audio_api)>;

std::mutex g_audio_mutex;
std::shared_ptr<AudioSystem> g_audio;
AudioRegistrations g_audio_registrations;
cp0::audio::RuntimeLifecycle g_audio_lifecycle;

} // namespace

extern "C" void init_audio(void)
{
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    if (!g_audio_lifecycle.begin_init()) return;
    try {
    auto active_audio = std::make_shared<AudioSystem>();
    if (!g_audio_registrations.replace(
        cp0::bind_signal(cp0_signal_audio_api,
                         [active_audio](std::list<std::string> arg,
                                 std::function<void(int, std::string)> callback) {
                             try {
                                 active_audio->api_call(std::move(arg), callback);
                             } catch (...) {
                                 cp0::audio::invoke_callback(
                                     callback, -1, "audio backend failure\n");
                             }
                         }))) {
        g_audio_lifecycle.rollback_init();
        return;
    }
    g_audio = std::move(active_audio);
    g_audio_lifecycle.commit_init();
    } catch (...) {
        g_audio_registrations.reset();
        g_audio.reset();
        g_audio_lifecycle.rollback_init();
    }
}

extern "C" void deinit_audio(void) noexcept
{
    // Join before dropping the audio signal. Otherwise a restore still inside
    // its deadline can write into the next AudioSystem after a restart.
    cp0_saved_volume_restore_stop();
    try {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        if (!g_audio_lifecycle.begin_shutdown()) return;
        g_audio_registrations.reset();
        g_audio.reset();
    } catch (...) {
    }
}
