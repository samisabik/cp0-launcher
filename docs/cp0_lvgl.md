# cp0_lvgl Interface Reference

This document is the public contract for `ext_components/cp0_lvgl`. New pages and extension modules can use it as their first reference. When fixing a bug, changing behavior, or checking a platform difference, inspect the actual implementation under `src/` and the matching `*_contract.*` files. Device and SDL backends share signal names and argument formats, but their underlying capabilities can differ.

## Quick Start

Important headers:

- `include/hal_lvgl_bsp.h`: C++ signal declarations and `cp0_file_path`.
- `include/signal_register_plan.h`: the complete `cp0_signal_*` signature list.
- `include/cp0_lvgl_app.h`: C ABI for resource paths, network, Wi-Fi, shutdown, sudo, battery, backlight, and time text.
- `include/cp0_lvgl_app_runner.hpp`: LVGL runner.
- `include/ui_app_page.hpp`: page bases and page startup.

Signals are global eventpp `CallbackList` objects. Request signals normally take `std::list<std::string>`: element zero is the command and the final argument is usually `std::function<void(int, std::string)>`. A callback receives `(code, data)`; `code == 0` is success and a negative code is an error. `data` may be empty, text, line records, or binary struct bytes. Do not assume the callback runs on the calling thread, and do not let callback exceptions escape.

Call `cp0_lvgl_init()` before using services; `cp0_lvgl_run()` does this automatically. Kconfig options named `CONFIG_CP0_LVGL_INIT_*` select services and `cp0_init_plan` controls their order. A disabled service has no usable implementation.

```cpp
cp0_signal_wifi_api({"Status"}, [](int code, std::string payload) {
    if (code != 0) return;
    // payload: connected:ssid:ip:signal:ethernet
});
```

Prefer the C wrappers in `cp0_lvgl_app.h` when one exists; they validate arguments and decode wire data.

## LVGL and Page API

```cpp
struct Cp0LvglRunOptions {
    std::function<void()> after_resource_init;
    std::function<bool()> setup;
    std::function<bool()> should_quit;
    std::function<void()> teardown;
};
int cp0_lvgl_run(Cp0LvglRunOptions options = {});
```

The runner supports one successful run per process. Its order is `lv_init`, `cp0_lvgl_init`, `after_resource_init`, `setup`, the LVGL timer loop, `teardown`, and service cleanup. A false `setup`, an exception, or a missing display returns 1. `should_quit` ends the loop.

`AppPageRoot` owns an LVGL screen and input group. `AppPage` adds the standard content container `ui_APP_Container` and top bar; `reclaim_top_bar()` hides the bar and grows the container to the full screen. Load a page with `cp0_lvgl_start_app_page(AppPageRoot&)`. Create and destroy page objects on the LVGL thread. The standard top bar is 20 px high.

## Public C ABI

Declarations are in `include/cp0_lvgl_app.h` and `include/hal_lvgl_bsp.h`. Unless noted otherwise, 0 means success and a negative value means failure.

### Paths

```c
void cp0_lvgl_init(void);
const char *cp0_file_path_c(const char *file);
```

`cp0_file_path_c` resolves logical names such as `applications`, `launcher_settings`, `home_dir`, `lock_file`, `keyboard_device`, `keyboard_map`, and `share/images/`, `share/audio/`, or font resources. The returned C string belongs to a thread-local cache; copy it if it must outlive the call. C++ callers use `std::string cp0_file_path(std::string)` from `hal_lvgl_bsp.h`.

### Network and Wi-Fi

```c
int cp0_network_list(cp0_netif_info_t *entries, int max_entries, int *out_count);
int cp0_wifi_status_read(cp0_wifi_status_t *status);
int cp0_wifi_scan(cp0_wifi_ap_t *entries, int max_entries);
int cp0_wifi_connect(const char *ssid, const char *password);
int cp0_wifi_connect_hidden(const char *ssid, const char *password);
int cp0_wifi_profile_forget(const char *ssid);
int cp0_wifi_profile_exists(const char *ssid);
int cp0_wifi_disconnect_active(void);
int cp0_wifi_radio_enabled(void);
int cp0_wifi_radio_set_enabled(int enabled);
```

`cp0_wifi_scan` returns an AP count or `CP0_WIFI_ERROR_INVALID=-1`, `RADIO_OFF=-2`, `AUTH=-3`, `NOT_FOUND=-4`, `IP_CONFIG=-5`, `SERVICE=-6`, or `TIMEOUT=-7`. `signal` is normally RSSI dBm; legacy backends may use 0..100.

### System

`cp0_system_shutdown` requests a power-off through the `Shutdown` process command. `cp0_desktop_exec_is_safe(exec, reason, reason_size)` returns 1 when a desktop entry's `Exec` passes the launch policy and otherwise 0 with a reason. Processes are started and stopped through `cp0_signal_process_api`.

### Sudo C API

`cp0_sudo_run_argv_async_ex` accepts an argv, a callback-thread selection, output callback, completion callback, user pointer, `auth_timeout_ms`, `exec_timeout_ms`, and an output `request_id`. `cp0_sudo_cancel(request_id)` is idempotent while the id is retained; unknown or expired ids return `-ENOENT`.

`CP0_SUDO_CALLBACK_LVGL` delivers output and completion on the LVGL thread; `CP0_SUDO_CALLBACK_WORKER` calls them on a worker. Output is ordered and LVGL delivery has bounded backpressure, so callbacks must return quickly. Completion results are `CP0_SUDO_RESULT_SUCCESS`, `AUTH_FAILED`, `EXEC_FAILED`, `CANCELLED`, or `TIMED_OUT`, together with the process exit code.

### Battery, backlight, and time

`cp0_battery_read` reads the latest background snapshot and performs no hardware I/O; use it only when `valid == 1`. Battery units are mV, mA, 0.1 C, mAh, and percent SOC. `cp0_backlight_write` writes a raw brightness value and returns the value written or -1. `cp0_time_str` writes the current time as `HH:MM`.

## Complete `cp0_signal_*` Usage

All `args` below are `std::list<std::string>` with the command at element zero. Request callbacks use `void(int code, std::string data)` unless a different signature is shown.

### Audio

`cp0_signal_audio_api` controls the default PulseAudio sink: `VolumeRead`, `VolumeWrite 0..100`, `DefaultSink` (sink name, or code -1 with empty data when the backend has no real sink), `MuteRead`, and `MuteToggle`. Volume and mute values are returned as decimal text; a failed mixer call returns code -1.

### PTY

`cp0_signal_pty_api` commands are `Open executable [columns] [rows] [argv...]` (default 80x24, returns a numeric handle), `Read handle [max_read]` (default 4096, max 1 MiB; code is byte count), `Write handle data` (code is byte count), `CheckChild handle` (code 0 running, 1 exited; data is exit code), and `Close handle`. Handles are not PIDs. Linux children use `TERM=vt100` and may drop privileges according to `run_as_user`; teardown kills remaining children.

### Configuration

`cp0_signal_config_api` uses `$HOME/.config/cardputerzero/config.json`, up to 64 entries, 63-byte keys, and 255-byte values. Commands are `Save`, `GetInt key fallback`, `SetInt key value`, `GetStr key fallback`, `SetStr key value`, and `SetManyAndSave key value [key value...]`. Set operations change memory; call `Save` to persist, or use the transaction command for atomic update-and-save.

### Filesystem signal: `cp0_signal_filesystem_api(args, cb)`

Commands are `Path logical_name`, `DirList path`, `Exists path`, `ReadFile path [max_bytes]`, `EnsureDirForUser path`, `WatchStart path`, `WatchPoll handle`, and `WatchStop handle`. `Exists` returns `1/0`; `WatchStart` returns a handle that may only be passed to `WatchPoll` and `WatchStop`; `DirList` returns one `D<TAB>name` or `F<TAB>name` line per entry with `%`, TAB, LF, and CR percent-encoded. Empty or NUL-containing paths are invalid. Unknown commands return code -2.

### Wi-Fi

Commands are `Status`, `Scan [0..32]`, `Connect ssid [password]`, `ConnectHidden ssid [password]`, `ProfileForget ssid`, `ProfileExists ssid`, `ProfileDisconnectActive`, `RadioEnabled`, and `RadioSetEnabled on|off|1|0|true|false`. Status data is `connected:ssid:ip:signal:ethernet`. Scan data has one `ssid:signal:security:in_use:saved` record per line, with backslash, colon, LF, and CR escaped. Prefer the C wrappers for decoding.

### Bluetooth and Agent

`cp0_signal_bt_api` supports `BtStatus`, `BtList [max=16]`, `BtConnectedList [max=16]`, `BtScan [max=16]`, `BtReset`, `BtPower 0|1`, `BtAlias name`, `BtDiscoverable 0|1`, `BtDiscoveryStart`, `BtDiscoveryStop`, `BtPair address`, `BtCancelPairing address`, `BtConnect address`, `BtDisconnect address`, and `BtRemove address`. Addresses must be `XX:XX:XX:XX:XX:XX`. Status data is `powered<TAB>address<TAB>discoverable<TAB>alias`; device lines are `address<TAB>rssi<TAB>connected<TAB>paired<TAB>trusted<TAB>name`.

`BtScan`, pairing, and connection operations can complete asynchronously.

`cp0_signal_bt_agent` has signature `(uint64_t id, std::string method, std::string device, std::string value, std::function<void(bool accepted, std::string text)> reply)`. It originates on a BlueZ worker thread. Marshal UI work to the LVGL/event loop, then call `reply` exactly once.

### Settings

`cp0_signal_settings_api` commands are `BacklightRead`, `BacklightMax`, `BacklightWrite value`, `TimeStr`, `GpioSet name 0|1`, and `GpioGet name`. GPIO names are `GROVE5V`/`extport_usb`, `EXT5V`/`extport_5vout`, and `BACKLIGHT`. Integer results are returned as decimal text; `TimeStr` is `HH:MM`.

### Process, OS information, and time

`cp0_signal_process_api` commands are `ExecBlocking executable keep_root`, `Spawn executable keep_root`, `Stop pid`, `CaptureArgv argv...`, `DesktopExecIsSafe executable`, `Shutdown`, and `Reboot`. `keep_root` is 0/1. `Shutdown` and `Reboot` run `systemctl poweroff|reboot --check-inhibitors=yes`, so a block inhibitor (a package install, an update) refuses them; on refusal the code is the systemctl exit status and data is its first output line. `Spawn` returns a PID; `CaptureArgv` returns stdout.

`cp0_signal_osinfo_api` and `cp0_signal_timedate_api` share one service. Commands are `SystemUpgradeStart` (starts or rejoins the single system upgrade job and returns its id), `UpdateJobStatus id` (`running`, a progress line, `succeeded:stage`, or `failed:stage:code`; a finished job is reported once), and `NtpGet` (code 1 when automatic time sync is enabled, 0 when disabled, negative on error). The SDL backend reports the upgrade as unsupported and NTP as disabled.

### Battery and screenshot

- `cp0_signal_bq27220_api`: `Read` returns encoded `cp0_battery_info_t`.
- `cp0_signal_screenshot_api`: `Save directory` writes a BMP and returns its complete path.

### Sudo signals

`cp0_signal_sudo_argv_async` takes `(argv, auth_timeout_ms, exec_timeout_ms, complete, started)`. `started(ret, request_id)` arrives first; on success `ret == 0` and the id is nonzero. `complete(result_code, exit_code)` then arrives once. Cancel with `cp0_signal_sudo_cancel(request_id, done)`; `done(0)` means cancellation was accepted.

`cp0_signal_system_admin_async` has the same signature but only accepts `NtpSet 0|1`, `TimezoneSet zone`, `TailscaleSet 0|1`, `SshSet 0|1` (`systemctl enable|disable --now ssh`), `HostnameSet name` (1..63 of `A-Za-z0-9-`, no leading or trailing `-`; also rewrites the `127.0.1.1` line of `/etc/hosts`), `MicGainSet 0..14`, and `TimeSet YYYY-MM-DD HH:MM:SS`. The allow-list lives in `src/cp0_admin_command.hpp`. Invalid arguments call only `started(-EINVAL, 0)` and do not produce a completion callback. Keep request ids and cancel them when a UI workflow leaves or times out.

## Change and Debug Rules

1. Read this document and `include/signal_register_plan.h` before changing a call site.
2. When changing an interface, update this document, the matching contract, C wrapper, call sites, and tests.
3. For bug fixes, inspect the implementation and contract files; verify callback threads, initialization, teardown, background workers, and wire escaping.
4. Run `ext_components/cp0_lvgl/tests/run_tests.sh`. Changes involving Linux commands, BlueZ, framebuffer, or sudo also require target-platform validation.
