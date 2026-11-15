/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launcher_media_controls.h"

#include "hal_lvgl_bsp.h"
#include "model/brightness_operation.hpp"
#include "model/launcher_media_model.hpp"
#include "model/setup_value_policy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace {

LauncherMediaControlsModel model;

/* Volume cached at launcher startup. Every successful write (and every
 * authoritative mixer read routed through update_cached_volume()) refreshes
 * it, so shortcuts and Settings render one value instead of racing the mixer. */
std::atomic<int> g_cached_volume{-1};

constexpr int kStartupVolumeReadAttempts = 5;
constexpr std::chrono::milliseconds kStartupVolumeReadInterval{400};

int read_mixer_volume()
{
    int volume = -1;
    try {
        cp0_signal_audio_api({"VolumeRead"}, [&](int code, std::string data) {
            int parsed = 0;
            if (code == 0 && LauncherMediaControlsModel::parse_percent(data, parsed)) volume = parsed;
        });
    } catch (...) {
        return -1;
    }
    return volume;
}

/* Reads the mixer on a worker thread because a PulseAudio session can take up
 * to two seconds to answer, and the startup path must not block the first
 * frame. WirePlumber applies the speaker port's stored/default volume while
 * the card comes up, which can land after the first successful read, so the
 * value is published only once two consecutive reads agree. If that never
 * happens, the last valid reading is used rather than leaving the cache empty.
 * The worker is joinable: launcher teardown joins it before the audio signal
 * is released, and the static destructor joins as a last resort. */
class StartupVolumeCache
{
public:
    ~StartupVolumeCache() { stop(); }

    void start()
    {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = false;
        }
        worker_ = std::thread([this] {
            try {
                run();
            } catch (...) {
            }
        });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

private:
    bool stop_requested()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stop_;
    }

    bool wait_for_next_attempt()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, kStartupVolumeReadInterval, [this] { return stop_; });
        return stop_;
    }

    void publish(int volume)
    {
        g_cached_volume.store(LauncherMediaControlsModel::clamp_percent(volume));
    }

    void run()
    {
        int previous = -1;
        int last_valid = -1;
        for (int attempt = 0; attempt < kStartupVolumeReadAttempts; ++attempt) {
            if (attempt > 0 && wait_for_next_attempt())
                return;
            if (stop_requested())
                return;

            const int volume = read_mixer_volume();
            if (volume < 0) {
                previous = -1;
                continue;
            }
            last_valid = volume;
            if (volume == previous) {
                publish(volume);
                return;
            }
            previous = volume;
        }
        if (last_valid >= 0 && !stop_requested())
            publish(last_valid);
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread worker_;
};

StartupVolumeCache g_startup_volume_cache;

std::atomic<bool> g_muted{false};
constexpr std::chrono::milliseconds kSaveDelay{1000};

/* Media keys must not block the LVGL thread: each PulseAudio call opens its own
 * session, and Save fsyncs the config and calls sync(). The key handler updates
 * the cached values and the OSD at once; this worker applies the latest mixer
 * target (older requests are dropped) and saves the config once the keys have
 * been idle for kSaveDelay. */
class MediaWorker
{
public:
    ~MediaWorker() { shutdown(false); }

    void request_volume(int target)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            volume_target_ = target;
            ensure_started_locked();
        }
        wake_.notify_all();
    }

    void request_mute_toggle()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++mute_toggles_;
            ensure_started_locked();
        }
        wake_.notify_all();
    }

    void request_save()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            save_pending_ = true;
            save_due_ = std::chrono::steady_clock::now() + kSaveDelay;
            ensure_started_locked();
        }
        wake_.notify_all();
    }

    bool newer_volume_pending()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return volume_target_ >= 0;
    }

    bool newer_mute_pending()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return mute_toggles_ > 0;
    }

    void flush()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return !busy_ && volume_target_ < 0 && mute_toggles_ == 0; });
        const bool save = save_pending_;
        save_pending_ = false;
        lock.unlock();
        if (save) save_config();
    }

    /* Teardown persists anything still pending; the static destructor only
     * joins, because the signal objects may already be gone at exit. */
    void shutdown(bool persist)
    {
        if (persist) flush();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
    }

private:
    void ensure_started_locked()
    {
        if (worker_.joinable()) return;
        worker_ = std::thread([this] {
            try {
                run();
            } catch (...) {
            }
        });
    }

    static void save_config()
    {
        try {
            cp0_signal_config_api({"Save"}, nullptr);
        } catch (...) {
        }
    }

    void run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::thread worker_;
    int volume_target_ = -1;
    int mute_toggles_ = 0;
    bool save_pending_ = false;
    bool busy_ = false;
    bool stop_ = false;
    std::chrono::steady_clock::time_point save_due_{};
};

MediaWorker g_media_worker;

int read_config_int(const char *key, int fallback)
{
    int value = fallback;
    cp0_signal_config_api({"GetInt", key, std::to_string(fallback)},
                          [&](int code, std::string data) {
                              int parsed = 0;
                              if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed))
                                  value = parsed;
                          });
    return value;
}

bool call_config(const std::list<std::string> &arguments)
{
    bool succeeded = false;
    cp0_signal_config_api(arguments, [&](int code, std::string) { succeeded = code == 0; });
    return succeeded;
}

// SetInt only updates the in-memory config; the write to disk is debounced.
bool set_config_int(const char *key, int value)
{
    if (!call_config({"SetInt", key, std::to_string(value)})) return false;
    g_media_worker.request_save();
    return true;
}

int read_volume()
{
    const int cached = g_cached_volume.load();
    if (cached >= 0) return LauncherMediaControlsModel::clamp_percent(cached);

    int volume = read_mixer_volume();
    if (volume < 0)
        volume = read_config_int("volume", model.volume_or(50));
    volume = LauncherMediaControlsModel::clamp_percent(volume);
    /* The first authoritative value becomes the cache, so a shortcut pressed
     * before the startup read completed cannot disagree with Settings. */
    g_cached_volume.store(volume);
    return volume;
}

// Mixer access below runs on the media worker only.
int mixer_mute_call(const char *command)
{
    int muted = -1;
    cp0_signal_audio_api({command}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) && (parsed == 0 || parsed == 1))
            muted = parsed;
    });
    return muted;
}

void apply_volume(int target, int previous)
{
    // A volume key also unmutes, as the hardware keys on most devices do.
    if (mixer_mute_call("MuteRead") == 1) {
        const int muted = mixer_mute_call("MuteToggle");
        if (muted >= 0 && !g_media_worker.newer_mute_pending()) g_muted.store(muted == 1);
    }
    int written = -1;
    cp0_signal_audio_api({"VolumeWrite", std::to_string(target)}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_percent(data, parsed)) written = parsed;
    });
    if (written < 0) {
        if (!g_media_worker.newer_volume_pending()) g_cached_volume.store(previous);
        return;
    }
    if (!g_media_worker.newer_volume_pending()) g_cached_volume.store(written);
    set_config_int("volume", written);
}

void MediaWorker::run()
{
    int applied_volume = g_cached_volume.load();
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (volume_target_ >= 0 || mute_toggles_ > 0) {
            const int target = volume_target_;
            const int toggles = mute_toggles_;
            volume_target_ = -1;
            mute_toggles_ = 0;
            busy_ = true;
            lock.unlock();
            if (toggles % 2 == 1) {
                const int muted = mixer_mute_call("MuteToggle");
                if (muted >= 0 && !newer_mute_pending()) g_muted.store(muted == 1);
            }
            if (target >= 0) {
                apply_volume(target, applied_volume);
                const int cached = g_cached_volume.load();
                if (cached >= 0) applied_volume = cached;
            }
            lock.lock();
            busy_ = false;
            idle_.notify_all();
            continue;
        }
        if (stop_) break;
        if (save_pending_) {
            if (std::chrono::steady_clock::now() >= save_due_) {
                save_pending_ = false;
                busy_ = true;
                lock.unlock();
                save_config();
                lock.lock();
                busy_ = false;
                idle_.notify_all();
                continue;
            }
            wake_.wait_until(lock, save_due_);
            continue;
        }
        wake_.wait(lock);
    }
}

int backlight_max()
{
    int maximum = 100;
    cp0_signal_settings_api({"BacklightMax"}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) && parsed > 0)
            maximum = parsed;
    });
    return maximum > 0 ? maximum : 100;
}

int read_backlight_raw(int maximum)
{
    int raw = -1;
    cp0_signal_settings_api({"BacklightRead"}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) && parsed >= 0)
            raw = parsed;
    });
    if (raw < 0) {
        const int fallback_index = setup_values::brightness_step_index(
            model.brightness_or(setup_values::kBrightnessMaxPercent));
        const int fallback = setup_values::brightness_step_value(fallback_index, maximum);
        raw = read_config_int(setup_values::kBrightnessConfigKey, fallback);
    }
    return raw;
}

int write_backlight_raw(int raw)
{
    int written = -1;
    cp0_signal_settings_api({"BacklightWrite", std::to_string(raw)}, [&](int code, std::string data) {
        int parsed = 0;
        if (code == 0 && LauncherMediaControlsModel::parse_int(data, parsed) && parsed >= 0)
            written = parsed;
    });
    return written;
}

int read_brightness()
{
    const int maximum = backlight_max();
    const int raw = read_backlight_raw(maximum);
    const int percent = setup_values::brightness_step_percent_from_raw(raw, maximum);
    model.set_brightness(percent);
    return percent;
}

int write_brightness(int previous_percent, int percent)
{
    const int target_index = setup_values::brightness_step_index(percent);
    const int maximum = backlight_max();
    const int raw = setup_values::brightness_step_value(target_index, maximum);

    const int written = write_backlight_raw(raw);
    if (written < 0) return previous_percent;
    const int previous_raw = setup_values::brightness_step_value(
        setup_values::brightness_step_index(previous_percent), maximum);
    if (!set_config_int(setup_values::kBrightnessConfigKey, written)) {
        cp0_signal_settings_api({"BacklightWrite", std::to_string(previous_raw)}, nullptr);
        return previous_percent;
    }
    const int applied_percent = setup_values::brightness_step_percent_from_raw(written, maximum);
    model.set_brightness(applied_percent);
    return applied_percent;
}

} // namespace

namespace launcher_media_controls {

bool has_cached_volume()
{
    return g_cached_volume.load() >= 0;
}

int cached_volume()
{
    return g_cached_volume.load();
}

void start_volume_cache()
{
    g_startup_volume_cache.start();
}

void stop_volume_cache()
{
    g_startup_volume_cache.stop();
    g_media_worker.shutdown(true);
}

void flush_pending()
{
    g_media_worker.flush();
}

void update_cached_volume(int percent)
{
    g_cached_volume.store(LauncherMediaControlsModel::clamp_percent(percent));
}

int adjust_volume(int delta_percent)
{
    const int current = read_volume();
    const int base = setup_values::round_volume_percent(current);
    const int delta = delta_percent > 0 ? VOLUME_STEP_PERCENT
                                       : delta_percent < 0 ? -VOLUME_STEP_PERCENT : 0;
    const int target = LauncherMediaControlsModel::clamp_percent(base + delta);
    g_cached_volume.store(target);
    g_muted.store(false);
    g_media_worker.request_volume(target);
    return target;
}

int adjust_brightness(int delta_percent)
{
    std::unique_lock<std::mutex> operation_lock(
        brightness_control::operation_mutex(), std::try_to_lock);
    if (!operation_lock.owns_lock()) {
        return setup_values::brightness_step_percent(
            setup_values::brightness_step_index(
                model.brightness_or(setup_values::kBrightnessMaxPercent)));
    }

    const int current = read_brightness();
    const int target = setup_values::brightness_step_percent_after(current, delta_percent);
    return write_brightness(current, target);
}

bool toggle_mute()
{
    // The OSD shows the predicted state; the worker corrects it from the mixer.
    const bool muted = !g_muted.load();
    g_muted.store(muted);
    g_media_worker.request_mute_toggle();
    return muted;
}

bool restore_startup_backlight()
{
    std::lock_guard<std::mutex> operation_lock(brightness_control::operation_mutex());
    const int maximum = backlight_max();
    const int saved = read_config_int(setup_values::kBrightnessConfigKey, maximum);
    const int target = saved > 0 ? std::min(saved, maximum) : maximum;
    const int written = write_backlight_raw(target);
    if (written <= 0 || written > maximum) return false;
    model.set_brightness(setup_values::brightness_step_percent_from_raw(written, maximum));
    return written == target;
}

int suspend_backlight()
{
    std::lock_guard<std::mutex> operation_lock(brightness_control::operation_mutex());
    const int maximum = backlight_max();
    int previous = read_backlight_raw(maximum);
    if (previous <= 0) {
        /* A zero reading carries no restore information; fall back to the
         * persisted step so waking up cannot leave the panel dark. */
        const int fallback_index = setup_values::brightness_step_index(
            model.brightness_or(setup_values::kBrightnessMaxPercent));
        previous = setup_values::brightness_step_value(fallback_index, maximum);
    }
    if (write_backlight_raw(0) < 0) return -1;
    return previous;
}

void restore_backlight(int raw)
{
    if (raw <= 0) return;
    std::lock_guard<std::mutex> operation_lock(brightness_control::operation_mutex());
    (void)write_backlight_raw(raw);
}

} // namespace launcher_media_controls
