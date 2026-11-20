/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cp0_enum_cast.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CP0_WIFI_AP_MAX 32
#define CP0_WIFI_SSID_MAX 64
#define CP0_BT_DEVICE_MAX 16
#define CP0_BT_NAME_MAX 64

typedef struct {
    int voltage_mv;
    int current_ma;
    int temperature_c10;
    int soc;
    int remain_mah;
    int full_mah;
    int flags;
    int avg_current_ma;
    int valid;
} cp0_battery_info_t;

typedef struct {
    char ssid[CP0_WIFI_SSID_MAX];
    int signal; /* 0..100 strength as NetworkManager reports it, or RSSI in dBm when negative */
    char security[32];
    int in_use;
    int saved;
} cp0_wifi_ap_t;

typedef struct {
    int connected;
    char ssid[CP0_WIFI_SSID_MAX];
    char ip[48];
    int signal; /* 0..100 strength as NetworkManager reports it, or RSSI in dBm when negative */
    int ethernet; /* 1 if a wired ethernet device is connected */
} cp0_wifi_status_t;

typedef struct {
    int powered;
    int discoverable;
    char address[24];
    char alias[CP0_BT_NAME_MAX];
} cp0_bt_status_t;

typedef struct {
    char name[CP0_BT_NAME_MAX];
    char address[24];
    int rssi;
    int connected;
    int paired;
    int trusted;
} cp0_bt_device_t;

typedef struct {
    char iface[32];
    char ipv4[16];
    char netmask[16];
    int is_up;
} cp0_netif_info_t;

typedef void *cp0_watcher_t;
typedef int cp0_pid_t;
typedef enum {
    CP0_SUDO_RESULT_SUCCESS = 0,
    CP0_SUDO_RESULT_AUTH_FAILED,
    CP0_SUDO_RESULT_EXEC_FAILED,
    CP0_SUDO_RESULT_CANCELLED,
    CP0_SUDO_RESULT_TIMED_OUT,
} cp0_sudo_result_t;

typedef enum {
    CP0_SUDO_CALLBACK_LVGL = 0,
    CP0_SUDO_CALLBACK_WORKER,
} cp0_sudo_callback_thread_t;

/* Output is delivered in order before completion. LVGL delivery uses bounded
 * backpressure, so callbacks should return promptly to avoid stalling the command. */
typedef void (*cp0_sudo_output_cb_t)(const char *data, size_t size, void *user);
typedef void (*cp0_sudo_complete_cb_t)(cp0_sudo_result_t result, int exit_code, void *user);

const char *cp0_file_path_c(const char *file);

int cp0_network_list(cp0_netif_info_t *entries, int max_entries, int *out_count);
typedef enum {
    CP0_WIFI_ERROR_INVALID = -1,
    CP0_WIFI_ERROR_RADIO_OFF = -2,
    CP0_WIFI_ERROR_AUTH = -3,
    CP0_WIFI_ERROR_NOT_FOUND = -4,
    CP0_WIFI_ERROR_IP_CONFIG = -5,
    CP0_WIFI_ERROR_SERVICE = -6,
    CP0_WIFI_ERROR_TIMEOUT = -7,
} cp0_wifi_error_t;
int cp0_wifi_status_read(cp0_wifi_status_t *status);
int cp0_wifi_scan(cp0_wifi_ap_t *entries, int max_entries);
int cp0_wifi_connect(const char *ssid, const char *password);
int cp0_wifi_connect_hidden(const char *ssid, const char *password);
int cp0_wifi_profile_forget(const char *ssid);
int cp0_wifi_profile_exists(const char *ssid);
int cp0_wifi_disconnect_active(void);
int cp0_wifi_radio_enabled(void);
int cp0_wifi_radio_set_enabled(int enabled);

void cp0_system_shutdown(void);

int cp0_sudo_run_argv_async_ex(const char *const *argv,
                               cp0_sudo_callback_thread_t callback_thread,
                               cp0_sudo_output_cb_t output_cb,
                               cp0_sudo_complete_cb_t complete_cb,
                               void *user,
                               /* Includes time spent waiting at the password prompt. */
                               int auth_timeout_ms,
                               /* Starts when the privileged command is launched. */
                               int exec_timeout_ms,
                               uint64_t *request_id);
/* Cancellation is idempotent while a request ID remains in the bounded recent
 * completion history. Returns -ENOENT only for an unknown or expired ID. */
int cp0_sudo_cancel(uint64_t request_id);
int cp0_desktop_exec_is_safe(const char *exec, char *reason, int reason_size);
/* Returns the latest background battery snapshot; never performs hardware I/O. */
cp0_battery_info_t cp0_battery_read(void);
int cp0_backlight_write(int val);
void cp0_time_str(char *buf, int buf_size);

#ifdef __cplusplus
}
#else
#define cp0_file_path(file) cp0_file_path_c(file)
#endif
