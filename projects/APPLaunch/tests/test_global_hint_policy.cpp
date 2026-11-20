/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/global_hint_policy.hpp"

#include "input_keys.h"

#include <cassert>
#include <string>

#ifndef KEY_RIGHTSHIFT
#define KEY_RIGHTSHIFT 54
#endif
#ifndef KEY_MUTE
#define KEY_MUTE 113
#endif
#ifndef KEY_VOLUMEUP
#define KEY_VOLUMEUP 115
#endif
#ifndef KEY_BRIGHTNESSDOWN
#define KEY_BRIGHTNESSDOWN 224
#endif
#ifndef KEY_SYSRQ
#define KEY_SYSRQ 99
#endif
#ifndef KEY_PRINT
#define KEY_PRINT 210
#endif

int main()
{
    const GlobalHintPolicy policy;

    assert(policy.action_for({KEY_BRIGHTNESSDOWN, true}) ==
           GlobalHintAction::BRIGHTNESS_DOWN);
    assert(policy.action_for({KEY_VOLUMEUP, false, true}) ==
           GlobalHintAction::VOLUME_UP);
    assert(policy.action_for({KEY_MUTE, false, true}) == GlobalHintAction::NONE);
    assert(policy.action_for({KEY_MUTE, true}) == GlobalHintAction::TOGGLE_MUTE);

    assert(policy.action_for({KEY_S, true, false, true, true}) ==
           GlobalHintAction::TAKE_SCREENSHOT);
    assert(policy.action_for({KEY_S, true, false, true, false}) ==
           GlobalHintAction::NONE);
    assert(policy.action_for({KEY_S, false, true, true, true}) ==
           GlobalHintAction::NONE);
    assert(policy.action_for({KEY_SYSRQ, true}) ==
           GlobalHintAction::TAKE_SCREENSHOT);
    assert(policy.action_for({KEY_PRINT, true}) ==
           GlobalHintAction::TAKE_SCREENSHOT);
    assert(policy.action_for({KEY_SYSRQ, false}) == GlobalHintAction::NONE);

    assert(policy.action_for({KEY_F10, true, false, false, false, true}) ==
           GlobalHintAction::RESET_LCD);
    assert(policy.action_for({KEY_F10, true}) == GlobalHintAction::NONE);
    assert(policy.action_for({KEY_F10, false, true, false, false, true}) ==
           GlobalHintAction::NONE);
    assert(policy.action_for({KEY_F10, false, false, false, false, true}) ==
           GlobalHintAction::NONE);
    assert(policy.action_for({KEY_F10, true, false, true, false, true}) ==
           GlobalHintAction::NONE);
    assert(policy.action_for({KEY_F10, true, false, false, true, true}) ==
           GlobalHintAction::NONE);

    assert(policy.action_for({KEY_LEFTSHIFT, true}) == GlobalHintAction::NONE);
    assert(policy.action_for({KEY_RIGHTSHIFT, true}) == GlobalHintAction::NONE);
    assert(policy.action_for({999, true}) == GlobalHintAction::NONE);

    assert(GlobalHintScreenshotPolicy::should_save(0));
    assert(!GlobalHintScreenshotPolicy::should_save(-1));
    assert(std::string(GlobalHintScreenshotPolicy::result_message(0, 0)) ==
           "Saved to ~/Pictures/Screenshots");
    assert(std::string(GlobalHintScreenshotPolicy::result_message(-1, 0)) ==
           "Screenshot failed");
    assert(std::string(GlobalHintScreenshotPolicy::result_message(0, -1)) ==
           "Screenshot failed");
    assert(GlobalHintScreenshotPolicy::saved_file_message(
               "/home/test/Pictures/Screenshots/scr_20260727_151000.bmp", "/home/test") ==
           "Saved: scr_20260727_151000.bmp\n~/Pictures/Screenshots");
    assert(GlobalHintScreenshotPolicy::saved_file_message(
               "/home/test2/Pictures/Screenshots/capture.bmp", "/home/test") ==
           "Saved: capture.bmp\n/home/test2/Pictures/Screenshots");
    assert(GlobalHintScreenshotPolicy::saved_file_message("capture.bmp", "/home/test") ==
           "Screenshot saved\ncapture.bmp");
}
