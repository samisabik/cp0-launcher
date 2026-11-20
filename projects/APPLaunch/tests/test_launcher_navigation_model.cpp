/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/launcher_navigation_model.hpp"

#include <cassert>

int main()
{
    LauncherPageLifecycleModel lifecycle;
    assert(lifecycle.state() == LauncherPageState::HOME);
    assert(!lifecycle.request_home());
    assert(!lifecycle.abort_app());
    assert(lifecycle.begin_app());
    assert(!lifecycle.begin_app());
    assert(lifecycle.abort_app());
    assert(lifecycle.state() == LauncherPageState::HOME);
    assert(!lifecycle.abort_app());
    assert(lifecycle.begin_app());
    assert(lifecycle.request_home());
    assert(!lifecycle.request_home());
    assert(!lifecycle.abort_app());
    lifecycle.cancel_home_request();
    assert(lifecycle.state() == LauncherPageState::APP_ACTIVE);
    assert(lifecycle.request_home());
    assert(lifecycle.complete_home());
    assert(!lifecycle.complete_home());
    assert(lifecycle.state() == LauncherPageState::HOME);
    assert(lifecycle.begin_app());
    lifecycle.stop();
    assert(lifecycle.state() == LauncherPageState::STOPPED);
    assert(!lifecycle.abort_app());
    assert(!lifecycle.request_home());
    assert(!lifecycle.begin_app());

    return 0;
}
