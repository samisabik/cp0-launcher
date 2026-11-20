/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "hal_lvgl_bsp.h"
#include "sdl_lvgl.h"
#include "../cp0_audio_api_contract.hpp"
#include "../cp0_audio_runtime_lifecycle.hpp"
#include "../cp0_signal_registration.hpp"

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {

class AudioSystem
{
public:
    using callback_t = std::function<void(int, std::string)>;

    void api_call(const std::list<std::string> &arg, callback_t callback)
    {
        cp0::audio::ApiRequest request;
        if (!cp0::audio::parse_api_request(arg, request)) {
            cp0::audio::invoke_callback(callback, -1, cp0::audio::invalid_api_request_message());
            return;
        }
        switch (request.command) {
        case cp0::audio::ApiCommand::VolumeRead:
            cp0::audio::invoke_callback(callback, 0, std::to_string(volume_));
            break;
        case cp0::audio::ApiCommand::VolumeWrite:
            volume_ = request.value;
            cp0::audio::invoke_callback(callback, 0, std::to_string(volume_));
            break;
        case cp0::audio::ApiCommand::DefaultSink:
            // No PulseAudio sink exists here. An empty failure keeps the startup
            // volume restore from treating this stub as a hardware speaker.
            cp0::audio::invoke_callback(callback, -1, "");
            break;
        case cp0::audio::ApiCommand::MuteRead:
            cp0::audio::invoke_callback(callback, 0, muted_ ? "1" : "0");
            break;
        case cp0::audio::ApiCommand::MuteToggle:
            muted_ = !muted_;
            cp0::audio::invoke_callback(callback, 0, muted_ ? "1" : "0");
            break;
        }
    }

private:
    static constexpr int kDefaultVolume = 39;

    int volume_ = kDefaultVolume;
    bool muted_ = false;
};

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
    auto audio = std::make_shared<AudioSystem>();
    if (!g_audio_registrations.replace(
        cp0::bind_signal(cp0_signal_audio_api,
                         [audio](std::list<std::string> arg,
                                 std::function<void(int, std::string)> callback) {
                             try {
                                 audio->api_call(std::move(arg), callback);
                             } catch (...) {
                                 cp0::audio::invoke_callback(
                                     callback, -1, "audio backend failure\n");
                             }
                         }))) {
        g_audio_lifecycle.rollback_init();
        return;
    }
    g_audio = std::move(audio);
    g_audio_lifecycle.commit_init();
    } catch (...) {
        g_audio_registrations.reset();
        g_audio.reset();
        g_audio_lifecycle.rollback_init();
    }
}

extern "C" void deinit_audio(void) noexcept
{
    // Same stop as the device backend: the restore worker is shared, and a
    // restart must not inherit a thread still writing the previous volume.
    cp0_saved_volume_restore_stop();
    try {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        if (!g_audio_lifecycle.begin_shutdown()) return;
        g_audio_registrations.reset();
        g_audio.reset();
    } catch (...) {
    }
}
