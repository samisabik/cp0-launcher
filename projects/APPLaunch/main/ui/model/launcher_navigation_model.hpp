/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>

enum class LauncherPageState
{
    HOME,
    APP_ACTIVE,
    HOME_PENDING,
    STOPPED,
};

class LauncherPageLifecycleModel
{
public:
    bool begin_app();
    bool abort_app();
    bool request_home();
    bool complete_home();
    void cancel_home_request();
    void stop();

    LauncherPageState state() const;

private:
    LauncherPageState state_ = LauncherPageState::HOME;
};
