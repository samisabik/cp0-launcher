/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "launcher_navigation_model.hpp"

bool LauncherPageLifecycleModel::begin_app()
{
    if (state_ != LauncherPageState::HOME) return false;
    state_ = LauncherPageState::APP_ACTIVE;
    return true;
}

bool LauncherPageLifecycleModel::abort_app()
{
    if (state_ != LauncherPageState::APP_ACTIVE) return false;
    state_ = LauncherPageState::HOME;
    return true;
}

bool LauncherPageLifecycleModel::request_home()
{
    if (state_ != LauncherPageState::APP_ACTIVE) return false;
    state_ = LauncherPageState::HOME_PENDING;
    return true;
}

bool LauncherPageLifecycleModel::complete_home()
{
    if (state_ != LauncherPageState::HOME_PENDING) return false;
    state_ = LauncherPageState::HOME;
    return true;
}

void LauncherPageLifecycleModel::cancel_home_request()
{
    if (state_ == LauncherPageState::HOME_PENDING)
        state_ = LauncherPageState::APP_ACTIVE;
}

void LauncherPageLifecycleModel::stop()
{
    state_ = LauncherPageState::STOPPED;
}

LauncherPageState LauncherPageLifecycleModel::state() const
{
    return state_;
}
