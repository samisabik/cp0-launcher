# cp0_lvgl Work Entry

## Read First

- Public interfaces, all `cp0_signal_*` commands, arguments, callback results, and thread rules: read [`docs/cp0_lvgl.md`](../../docs/cp0_lvgl.md).
- Authoritative signal signatures: `include/signal_register_plan.h`.
- C ABI: `include/cp0_lvgl_app.h`; LVGL pages and runner: `include/cp0_lvgl_app_runner.hpp` and `include/ui_app_page.hpp`.
- When fixing a bug, changing behavior, or checking a platform difference, inspect the relevant implementation under `src/` and its `*_contract.*` files, then compare the device and SDL backends.

## Module Boundary

`cp0_lvgl` provides the LVGL runner, reusable page bases, and eventpp callback-list services for audio volume, PTY, configuration, filesystem, Wi-Fi, Bluetooth, settings, process execution, OS information, time, battery, screenshots, and sudo. Whether a service is installed is controlled by `CONFIG_CP0_LVGL_INIT_*`; never assume a disabled service exists.

Request signals normally take `std::list<std::string>` with the command name at element zero and a `std::function<void(int, std::string)>` callback. `code == 0` means success; interpret `data` using the wire format documented in [`docs/cp0_lvgl.md`](../../docs/cp0_lvgl.md). Callbacks may run on worker threads, so UI code must marshal back to the LVGL thread. Stop background threads, timers, PTYs, and sudo requests before service teardown.

## Public Interface Quick Reference

- Runner: `cp0_lvgl_init()`, `cp0_lvgl_run()`; pages: `AppPageRoot`, `AppPage`, and `cp0_lvgl_start_app_page()`.
- C wrappers: `cp0_file_path_c`, `cp0_network_list`, `cp0_wifi_*`, `cp0_system_shutdown`, `cp0_desktop_exec_is_safe`, `cp0_sudo_run_argv_async_ex`, `cp0_sudo_cancel`, `cp0_battery_read`, `cp0_backlight_write`, and `cp0_time_str`. Full declarations are in `include/cp0_lvgl_app.h`.
- Request/response signals: `cp0_signal_audio_api`, `cp0_signal_pty_api`, `cp0_signal_config_api`, `cp0_signal_filesystem_api`, `cp0_signal_wifi_api`, `cp0_signal_bt_api`, `cp0_signal_settings_api`, `cp0_signal_process_api`, `cp0_signal_osinfo_api`, `cp0_signal_timedate_api`, `cp0_signal_bq27220_api`, and `cp0_signal_screenshot_api`.
- Async-specialized signals: `cp0_signal_bt_agent(uint64_t, method, device, value, reply)`, `cp0_signal_sudo_argv_async(args, auth_timeout_ms, exec_timeout_ms, complete, started)`, `cp0_signal_sudo_cancel(request_id, done)`, and `cp0_signal_system_admin_async(args, auth_timeout_ms, exec_timeout_ms, complete, started)`.
- Bluetooth Agent requests originate on a worker thread; sudo calls `started` before `complete`; neither callback may directly manipulate LVGL objects.

For command details, argument counts, return encoding, and platform differences, use the “Complete `cp0_signal_*` Usage” section in [`docs/cp0_lvgl.md`](../../docs/cp0_lvgl.md).

## Modification Rules

- A new signal exposed to external modules must be declared in `include/signal_register_plan.h` with `def_hal_fun`, and must include registration, teardown, device and SDL behavior, and tests. Example:

  `def_hal_fun(void(std::list<std::string>, std::function<void(int, std::string)>), cp0_signal_xxx_api)`

- When changing an argument or return format, update `docs/cp0_lvgl.md`, the corresponding contract, C wrappers, all call sites, and tests.
- Run `ext_components/cp0_lvgl/tests/run_tests.sh` for module regression. Changes involving Linux commands, BlueZ, framebuffer, or sudo also require target-platform validation.
