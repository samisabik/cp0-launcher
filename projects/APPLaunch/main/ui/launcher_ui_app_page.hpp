/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ui.h"
#include "keyboard_input.h"
#include "input_keys.h"
#include "ui_app_page.hpp"

#include <pwd.h>
#include <string>
#include <unistd.h>

// The launcher home screen uses the same page shell as standalone apps, with a
// shell prompt (user@host:~$) as its title.
class home_base : public AppPage
{
public:
    home_base()
    {
        set_page_title(prompt_title());
    }

    static std::string prompt_title()
    {
        const passwd *account = getpwuid(getuid());
        char host[64] = {};
        if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0')
            std::snprintf(host, sizeof(host), "cp0");
        return std::string(account && account->pw_name ? account->pw_name : "user") + "@" + host + ":~$";
    }

    lv_obj_t *content_container() const { return ui_APP_Container; }
    lv_obj_t *top_container() const { return root_screen_; }
    void use_bold_home_title_font() {}
};
