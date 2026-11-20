/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct key_item;

void ui_lockscreen_init(void);
void ui_lockscreen_deinit(void);
int ui_lockscreen_filter_key(const struct key_item *item);
/* Includes the exit animation, until the working screen is fully visible. */
int ui_lockscreen_is_active(void);
/* Locks immediately, as if the idle timeout had fired. */
void ui_lockscreen_lock_now(void);
void ui_lockscreen_set_foreground(int foreground);

#ifdef __cplusplus
}
#endif
