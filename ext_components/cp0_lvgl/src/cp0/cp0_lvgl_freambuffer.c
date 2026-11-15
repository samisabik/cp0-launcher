/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "hal_lvgl_bsp.h"
#include "lvgl/lvgl.h"
#include "commount.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cp0_lvgl.h"
#include "../cp0_display_screenshot_contract.hpp"
#include <pthread.h>
#include <fcntl.h>
#include <stdbool.h>

/* The panel sits behind DRM fbdev emulation, which pushes mmap writes to the
 * SPI panel from deferred I/O on a timer. fsync runs that pending flush right
 * away, so each finished frame reaches the panel without waiting for the timer. */
static int s_fb_sync_fd = -1;
static bool s_fb_frame_flushed = false;

static void fb_flush_finish_cb(lv_event_t *e)
{
    (void)e;
    s_fb_frame_flushed = true;
}

static void fb_refr_ready_cb(lv_event_t *e)
{
    (void)e;
    if (!s_fb_frame_flushed || s_fb_sync_fd < 0) return;
    s_fb_frame_flushed = false;
    fsync(s_fb_sync_fd);
}

static int find_st7789v_fbdev(char *dev_path, size_t buf_size)
{
    if (dev_path == NULL || buf_size == 0)
        return -1;

    if (access("/dev/fb_lcd", F_OK) == 0) {
        snprintf(dev_path, buf_size, "/dev/fb_lcd");
        return 0;
    }

    FILE *fp = fopen("/proc/fb", "r");
    if (fp != NULL) {
        char line[256];
        int fb_num = -1;
        while (fgets(line, sizeof(line), fp) != NULL) {
            if (strstr(line, "fb_st7789v") != NULL || strstr(line, "st7789") != NULL) {
                if (sscanf(line, "%d", &fb_num) == 1)
                    break;
            }
        }
        fclose(fp);
        if (fb_num >= 0) {
            snprintf(dev_path, buf_size, "/dev/fb%d", fb_num);
            return 0;
        }
    }

    snprintf(dev_path, buf_size, "/dev/fb0");
    return 0;
}

void init_freambuffer_disp()
{
    static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&init_mutex);
    if (lv_display_get_default() != NULL) {
        pthread_mutex_unlock(&init_mutex);
        return;
    }
#if LV_USE_LINUX_DRM
    const char *device = getenv("LV_LINUX_DRM_DEVICE");
    char *detected_device = NULL;
    if (device == NULL || device[0] == '\0') {
        detected_device = lv_linux_drm_find_device_path();
        device = detected_device;
    }

    if (device == NULL || device[0] == '\0') {
        fprintf(stderr, "Failed to find a connected DRM device\n");
        lv_free(detected_device);
        pthread_mutex_unlock(&init_mutex);
        return;
    }

    printf("Using DRM device: %s\n", device);
    lv_display_t *disp = lv_linux_drm_create();
    if (disp == NULL) {
        fprintf(stderr, "Failed to create DRM display\n");
        lv_free(detected_device);
        pthread_mutex_unlock(&init_mutex);
        return;
    }

    if (lv_linux_drm_set_file(disp, device, -1) != LV_RESULT_OK) {
        fprintf(stderr, "Failed to initialize DRM device: %s\n", device);
        lv_display_delete(disp);
        lv_free(detected_device);
        pthread_mutex_unlock(&init_mutex);
        return;
    }

    lv_free(detected_device);

    printf("DRM resolution: %dx%d\n",
           (int)lv_display_get_horizontal_resolution(disp),
           (int)lv_display_get_vertical_resolution(disp));
#else
    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp == NULL) {
        printf("Failed to create fbdev display!\n");
        pthread_mutex_unlock(&init_mutex);
        return;
    }

    const char *device = getenv("LV_LINUX_FBDEV_DEVICE");
    char fbdev[64] = {0};
    if (device == NULL || device[0] == '\0') {
        find_st7789v_fbdev(fbdev, sizeof(fbdev));
        device = fbdev;
    }

    setenv("APPLAUNCH_LINUX_FBDEV_DEVICE", device, 1);
    setenv("LV_LINUX_FBDEV_DEVICE", device, 1);
    printf("Using framebuffer device: %s\n", device);
    lv_linux_fbdev_set_file(disp, device);
    s_fb_sync_fd = open(device, O_RDWR | O_CLOEXEC);
    if (s_fb_sync_fd >= 0) {
        lv_display_add_event_cb(disp, fb_flush_finish_cb, LV_EVENT_FLUSH_FINISH, NULL);
        lv_display_add_event_cb(disp, fb_refr_ready_cb, LV_EVENT_REFR_READY, NULL);
    }

    lv_coord_t w = lv_display_get_horizontal_resolution(disp);
    lv_coord_t h = lv_display_get_vertical_resolution(disp);
    printf("Framebuffer resolution: %dx%d\n", w, h);
#endif
    pthread_mutex_unlock(&init_mutex);
}
