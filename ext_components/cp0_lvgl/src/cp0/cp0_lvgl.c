/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "hal_lvgl_bsp.h"
#include "lvgl/lvgl.h"
#include "commount.h"
#include "cp0_lvgl.h"
#include "../cp0_init_plan.h"
#include <pthread.h>

static int run_init_step(cp0_init_step_t step)
{
    switch (step) {
    case CP0_INIT_STEP_ENV: init_lvgl_env(); break;
    case CP0_INIT_STEP_EVENT: init_lvgl_event(); break;
    case CP0_INIT_STEP_FILESYSTEM:
        init_filesystem();
        break;
    case CP0_INIT_STEP_CONFIG:
        init_config();
        break;
    case CP0_INIT_STEP_PTY:
        init_pty();
        break;
    case CP0_INIT_STEP_DISPLAY:
        init_freambuffer_disp();
        return lv_display_get_default() != NULL;
    case CP0_INIT_STEP_INPUT: init_input(); break;
    case CP0_INIT_STEP_AUDIO:
        init_audio();
        break;
    case CP0_INIT_STEP_PROCESS:
        init_process();
        break;
    case CP0_INIT_STEP_SUDO:
        init_sudo_signals();
        break;
    case CP0_INIT_STEP_OSINFO:
        init_osinfo();
        break;
    case CP0_INIT_STEP_SCREENSHOT:
        init_screenshot();
        break;
    case CP0_INIT_STEP_WIFI:
        init_wifi();
        break;
    case CP0_INIT_STEP_BLUETOOTH:
        init_bluetooth();
        break;
    case CP0_INIT_STEP_SETTINGS:
        init_settings();
        break;
    case CP0_INIT_STEP_BQ27220:
        init_bq27220();
        break;
    case CP0_INIT_STEP_SAVED_SETTINGS:
        init_lvgl_saved_settings();
        break;
    case CP0_INIT_STEP_BATTERY:
        init_battery();
        break;
    case CP0_INIT_STEP_COUNT:
        break;
    }
    return 1;
}

void cp0_lvgl_init(void)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    static int next_step = 0;

    pthread_mutex_lock(&mutex);
    while (next_step < CP0_INIT_STEP_COUNT) {
        if (!run_init_step((cp0_init_step_t)next_step))
            break;
        ++next_step;
    }
    pthread_mutex_unlock(&mutex);
}
