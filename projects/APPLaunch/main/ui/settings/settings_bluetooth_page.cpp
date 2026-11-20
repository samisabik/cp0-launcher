/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "settings_bluetooth_page.hpp"
#include "keyboard_text_input.hpp"
#include "cp0_enum_cast.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <system_error>
#include <utility>
#include <new>
#include <string_view>

namespace {

class settings_bluetooth_com {
public:
    struct StatusRecord {
        bool powered = false;
        std::string address;
        bool discoverable = false;
        std::string alias;
    };

    struct DeviceRecord {
        std::string address;
        int rssi = 0;
        bool connected = false;
        bool paired = false;
        bool trusted = false;
        std::string name;
    };

    enum class ValidationLimit : std::size_t {
        MaxAliasBytes     = CP0_BT_NAME_MAX - 1,
        MaxAddressBytes   = sizeof(((cp0_bt_status_t *)nullptr)->address) - 1,
        MaxDeviceNameBytes = CP0_BT_NAME_MAX - 1,
    };

    static bool parse_boolean(std::string_view value, bool &output)
    {
        if (value != "0" && value != "1") return false;
        output = value == "1";
        return true;
    }

    static bool parse_integer(std::string_view value, int &output)
    {
        if (value.empty()) return false;
        int parsed = 0;
        const char *begin = value.data();
        const char *end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed, 10);
        if (result.ec != std::errc{} || result.ptr != end) return false;
        output = parsed;
        return true;
    }

    static bool has_wire_control(std::string_view value)
    {
        for (const unsigned char byte : value) {
            if (byte < 0x20 || byte == 0x7f) return true;
        }
        return false;
    }

    static bool valid_utf8(std::string_view value)
    {
        std::size_t index = 0;
        while (index < value.size()) {
            const unsigned char first = static_cast<unsigned char>(value[index]);
            if (first <= 0x7f) {
                ++index;
                continue;
            }

            std::size_t width = 0;
            unsigned char second_min = 0x80;
            unsigned char second_max = 0xbf;
            if (first >= 0xc2 && first <= 0xdf) {
                width = 2;
            } else if (first == 0xe0) {
                width = 3;
                second_min = 0xa0;
            } else if (first >= 0xe1 && first <= 0xec) {
                width = 3;
            } else if (first == 0xed) {
                width = 3;
                second_max = 0x9f;
            } else if (first >= 0xee && first <= 0xef) {
                width = 3;
            } else if (first == 0xf0) {
                width = 4;
                second_min = 0x90;
            } else if (first >= 0xf1 && first <= 0xf3) {
                width = 4;
            } else if (first == 0xf4) {
                width = 4;
                second_max = 0x8f;
            } else {
                return false;
            }

            if (index + width > value.size()) return false;
            const unsigned char second = static_cast<unsigned char>(value[index + 1]);
            if (second < second_min || second > second_max) return false;
            for (std::size_t continuation = 2; continuation < width; ++continuation) {
                const unsigned char byte =
                    static_cast<unsigned char>(value[index + continuation]);
                if (byte < 0x80 || byte > 0xbf) return false;
            }
            index += width;
        }
        return true;
    }

    static bool valid_text_field(std::string_view value,
                                 std::size_t maximum_bytes,
                                 bool allow_empty = true)
    {
        if ((!allow_empty && value.empty()) || value.size() > maximum_bytes) return false;
        return !has_wire_control(value) && valid_utf8(value);
    }

    static bool valid_device_address(std::string_view address)
    {
        if (address.size() != 17) return false;
        for (std::size_t index = 0; index < address.size(); ++index) {
            if ((index + 1) % 3 == 0) {
                if (address[index] != ':') return false;
            } else if (!std::isxdigit(static_cast<unsigned char>(address[index]))) {
                return false;
            }
        }
        return true;
    }

    static bool split_fields(std::string_view record,
                             std::size_t expected_count,
                             std::vector<std::string_view> &fields)
    {
        fields.clear();
        std::size_t start = 0;
        while (true) {
            const std::size_t separator = record.find('\t', start);
            fields.emplace_back(record.substr(
                start,
                separator == std::string_view::npos ? std::string_view::npos : separator - start));
            if (separator == std::string_view::npos) break;
            start = separator + 1;
        }
        return fields.size() == expected_count;
    }

    static bool decode_status(std::string_view payload, StatusRecord &output)
    {
        std::vector<std::string_view> fields;
        if (!split_fields(payload, 4, fields)) return false;

        StatusRecord decoded;
        if (!parse_boolean(fields[0], decoded.powered) ||
            !parse_boolean(fields[2], decoded.discoverable))
            return false;
        if (fields[1].size() > static_cast<std::size_t>(ValidationLimit::MaxAddressBytes) ||
            has_wire_control(fields[1]))
            return false;
        if (decoded.powered && fields[1].empty()) return false;
        if (!fields[1].empty() && !valid_device_address(fields[1])) return false;
        if (!valid_text_field(fields[3], static_cast<std::size_t>(ValidationLimit::MaxAliasBytes)))
            return false;
        decoded.address.assign(fields[1]);
        decoded.alias.assign(fields[3]);
        output = std::move(decoded);
        return true;
    }

    static bool decode_device(std::string_view payload, DeviceRecord &output)
    {
        std::vector<std::string_view> fields;
        if (!split_fields(payload, 6, fields)) return false;
        DeviceRecord decoded;
        if (!valid_device_address(fields[0]) || !parse_integer(fields[1], decoded.rssi) ||
            !parse_boolean(fields[2], decoded.connected) ||
            !parse_boolean(fields[3], decoded.paired) ||
            !parse_boolean(fields[4], decoded.trusted) ||
            !valid_text_field(fields[5], static_cast<std::size_t>(ValidationLimit::MaxDeviceNameBytes)))
            return false;
        decoded.address.assign(fields[0]);
        decoded.name.assign(fields[5]);
        output = std::move(decoded);
        return true;
    }

    static bool decode_devices(std::string_view payload,
                                std::vector<DeviceRecord> &output,
                                int maximum_count = CP0_BT_DEVICE_MAX)
    {
        output.clear();
        if (maximum_count < 0 || maximum_count > CP0_BT_DEVICE_MAX) return false;
        if (payload.empty()) return true;
        if (payload.back() == '\n') {
            payload.remove_suffix(1);
            if (payload.empty() || payload.back() == '\n') return false;
        }
        std::size_t start = 0;
        while (start < payload.size()) {
            const std::size_t separator = payload.find('\n', start);
            const std::size_t end = separator == std::string_view::npos ? payload.size() : separator;
            const std::string_view line = payload.substr(start, end - start);
            if (line.empty() || static_cast<int>(output.size()) >= maximum_count) return false;
            DeviceRecord device;
            if (!decode_device(line, device)) return false;
            output.push_back(std::move(device));
            if (separator == std::string_view::npos) break;
            start = separator + 1;
        }
        return true;
    }

    static bool decode_device_list_reply(int code,
                                         std::string_view payload,
                                         std::vector<DeviceRecord> &output,
                                         int maximum_count = CP0_BT_DEVICE_MAX)
    {
        output.clear();
        if (code < 0 || code > maximum_count) return false;
        if (!decode_devices(payload, output, maximum_count)) return false;
        return static_cast<int>(output.size()) == code;
    }

    static bool valid_alias(std::string_view alias)
    {
        return valid_text_field(alias, static_cast<std::size_t>(ValidationLimit::MaxAliasBytes), false);
    }

    static bool success_without_payload(int code, std::string_view payload)
    {
        return code == 0 && (payload.empty() || payload == "ok");
    }

    static std::list<std::string> status_request() { return {"BtStatus"}; }

    static std::list<std::string> power_request(bool enabled)
    {
        return {"BtPower", enabled ? "1" : "0"};
    }

    static std::list<std::string> discoverable_request(bool enabled)
    {
        return {"BtDiscoverable", enabled ? "1" : "0"};
    }

    static bool alias_request(std::string_view alias, std::list<std::string> &request)
    {
        if (!valid_alias(alias)) return false;
        request = {"BtAlias", std::string(alias)};
        return true;
    }

    static bool list_request(bool connected_only, int maximum_count,
                             std::list<std::string> &request)
    {
        if (maximum_count < 1 || maximum_count > CP0_BT_DEVICE_MAX) return false;
        request = {connected_only ? "BtConnectedList" : "BtList", std::to_string(maximum_count)};
        return true;
    }

    static bool scan_request(int maximum_count, std::list<std::string> &request)
    {
        if (maximum_count < 1 || maximum_count > CP0_BT_DEVICE_MAX) return false;
        request = {"BtScan", std::to_string(maximum_count)};
        return true;
    }

    static bool discovery_start_request(std::list<std::string> &request)
    {
        request = {"BtDiscoveryStart"};
        return true;
    }

    static std::list<std::string> discovery_start_request()
    {
        return {"BtDiscoveryStart"};
    }

    static bool discovery_stop_request(std::list<std::string> &request)
    {
        request = {"BtDiscoveryStop"};
        return true;
    }

    static std::list<std::string> discovery_stop_request()
    {
        return {"BtDiscoveryStop"};
    }

    static bool device_request(const char *command,
                               std::string_view address,
                               std::list<std::string> &request)
    {
        if (!command || !valid_device_address(address)) return false;
        const std::string command_text(command);
        if (command_text != "BtPair" && command_text != "BtCancelPairing" && command_text != "BtConnect" &&
            command_text != "BtDisconnect" && command_text != "BtRemove")
            return false;
        request = {command_text, std::string(address)};
        return true;
    }
};

void copy_text(char *target, std::size_t capacity, std::string_view value)
{
    if (!target || capacity == 0) return;
    const std::size_t length = std::min(capacity - 1, value.size());
    if (length > 0) std::memcpy(target, value.data(), length);
    target[length] = '\0';
}

void copy_device_record(const settings_bluetooth_com::DeviceRecord &source,
                        cp0_bt_device_t &target)
{
    target = {};
    copy_text(target.address, sizeof(target.address), source.address);
    copy_text(target.name, sizeof(target.name), source.name);
    target.rssi = source.rssi;
    target.connected = source.connected ? 1 : 0;
    target.paired = source.paired ? 1 : 0;
    target.trusted = source.trusted ? 1 : 0;
}

bool is_agent_authorization_method(std::string_view method)
{
    return method == "RequestAuthorization" || method == "AuthorizeService";
}

bool valid_hostname(std::string_view value)
{
    if (value.empty() || value.size() > 63 || value.front() == '-' || value.back() == '-') return false;
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isalnum(c) != 0 || c == '-'; });
}

} // namespace

struct LvSettingBluetoothAliasPage3::ApiDispatchState {
    struct Result {
        std::weak_ptr<bool> lifetime;
        ApiHandler handler;
        ApiHandler stale_handler;
        uint64_t generation = 0;
        int code = -1;
        std::string data;
    };

    std::mutex mutex;
    bool stopped = false;
    std::deque<Result> pending;
};

void LvSettingBluetoothAliasPage3::enqueue_api_result(
    const std::shared_ptr<ApiDispatchState> &dispatch,
    const std::weak_ptr<bool> &lifetime,
    ApiHandler handler,
    ApiHandler stale_handler,
    uint64_t generation,
    int code,
    std::string data) noexcept
{
    if (!dispatch) return;
    try {
        std::lock_guard<std::mutex> lock(dispatch->mutex);
        if (dispatch->stopped || lifetime.expired()) return;
        dispatch->pending.push_back({lifetime,
                                     std::move(handler),
                                     std::move(stale_handler),
                                     generation,
                                     code,
                                     std::move(data)});
    } catch (...) {
    }
}

    LvSettingBluetoothPage3::LvSettingBluetoothPage3(lv_obj_t *parent,
                            const NodeIter &parent_node,
                            std::function<void()> back_callback,
                            LvSettingBluetoothListMode mode)
        : parent_node_(parent_node),
          mode_(mode)
{
        LeaveSelfPage = std::move(back_callback);
        create_ui(parent);
    }


    void LvSettingBluetoothPage3::AnimateNextIn(std::function<void()> animate_over_func)
{
        if (animate_over_func) animate_over_func();
    }

    void LvSettingBluetoothPage3::AnimateNextOut(std::function<void()> animate_over_func)
{
        if (animate_over_func) animate_over_func();
    }

    void LvSettingBluetoothPage3::LoadNextPage()
{}

    void LvSettingBluetoothPage3::LeaveNextPage()
{
        if (LeaveSelfPage) LeaveSelfPage();
    }


LvSettingBluetoothPage3::~LvSettingBluetoothPage3()
{
        restore_text_input_mode();
        if (agent_prompt_active_ && agent_request_.reply) {
            try { agent_request_.reply(false, {}); } catch (...) {}
        }
        agent_registration_.reset();
        if (keyboard_root_ && keyboard_event_dsc_) {
            lv_obj_remove_event_dsc(keyboard_root_, keyboard_event_dsc_);
            keyboard_event_dsc_ = nullptr;
        }
        if (api_timer_) {
            lv_timer_delete(api_timer_);
            api_timer_ = nullptr;
        }
        if (status_retry_timer_) {
            lv_timer_delete(status_retry_timer_);
            status_retry_timer_ = nullptr;
        }
        ++generation_;
        stop_scan();
        if (restore_discoverable_) {
            api_tasks_.start([] {
                try {
                    cp0_signal_bt_api(settings_bluetooth_com::discoverable_request(false), [](int, std::string) {});
                } catch (...) {
                }
            });
        }
        std::deque<AgentPromptRequest> queued_agents;
        {
            std::lock_guard<std::mutex> lock(api_dispatch_->mutex);
            api_dispatch_->stopped = true;
            api_dispatch_->pending.clear();
            queued_agents.swap(api_dispatch_->agent_events);
        }
        for (auto &request : queued_agents) {
            if (request.reply) {
                try { request.reply(false, {}); } catch (...) {}
            }
        }
        page_lifetime_.reset();
        api_tasks_.join_all();
        if (ComponensObj) {
            lv_anim_del(ComponensObj, nullptr);
            lv_obj_delete(ComponensObj);
            ComponensObj = nullptr;
        }
    }


    void LvSettingBluetoothPage3::create_ui(lv_obj_t *parent)
{
        if (!parent) return;

        ComponensObj = lv_obj_create(parent);
        if (!ComponensObj) return;
        lv_obj_set_size(ComponensObj,
                        CP0_ENUM_CAST_INT(LayoutMetric::ScreenW),
                        CP0_ENUM_CAST_INT(LayoutMetric::ScreenH));
        lv_obj_set_pos(ComponensObj, 0, 0);
        lv_obj_set_style_bg_color(ComponensObj, lv_color_hex(cp0::theme::kBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ComponensObj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(ComponensObj, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ComponensObj, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(ComponensObj, 0, LV_PART_MAIN);
        lv_obj_remove_flag(ComponensObj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(ComponensObj, LV_OBJ_FLAG_SCROLLABLE);
        DComponens::lvgl_bind_event(
            ComponensObj,
            LV_EVENT_KEY,
            nullptr,
            std::bind(&LvSettingBluetoothPage3::handle_key_event,
                      this,
                      std::placeholders::_1));
        keyboard_root_ = lv_screen_active();
        if (keyboard_root_ && LV_EVENT_KEYBOARD != 0) {
            keyboard_event_dsc_ = lv_obj_add_event_cb(
                keyboard_root_,
                keyboard_event_cb,
                static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD),
                this);
        }
        api_timer_ = lv_timer_create(api_result_timer_cb, 50, this);
        const std::weak_ptr<bool> lifetime = page_lifetime_;
        agent_registration_.replace(
            cp0_signal_bt_agent,
            [dispatch = api_dispatch_, lifetime](uint64_t id,
                                                  std::string method,
                                                  std::string device,
                                                  std::string hint,
                                                  std::function<void(bool, std::string)> reply) {
                if (!reply) return;
                if (lifetime.expired() || !dispatch) {
                    try { reply(false, {}); } catch (...) {}
                    return;
                }
                bool queued = false;
                try {
                    std::lock_guard<std::mutex> lock(dispatch->mutex);
                    if (!dispatch->stopped) {
                        dispatch->agent_events.push_back(
                            {id,
                             std::move(method),
                             std::move(device),
                             std::move(hint),
                             std::move(reply)});
                        queued = true;
                    }
                } catch (...) {
                }
                if (!queued) {
                    try { reply(false, {}); } catch (...) {}
                }
            });
        // BlueZ may still be starting when the page is opened. Retry only
        // unresolved status requests; a real powered-off adapter is handled
        // by the power warning and does not spin the request loop.
        status_retry_timer_ = lv_timer_create(status_retry_timer_cb, 1000, this);
        if (status_retry_timer_) lv_timer_pause(status_retry_timer_);

        render();
        request_status();
    }


    void LvSettingBluetoothPage3::enqueue_api_result(const std::shared_ptr<ApiDispatchState> &dispatch,
                                   LvSettingBluetoothPage3 *owner,
                                   const std::weak_ptr<bool> &lifetime,
                                   const ApiHandler &handler,
                                   const ApiHandler &stale_handler,
                                   uint64_t generation,
                                   int code,
                                   std::string data) noexcept
{
        if (!dispatch) return;
        try {
            std::lock_guard<std::mutex> lock(dispatch->mutex);
            if (dispatch->stopped) return;
            dispatch->pending.push_back(
                ApiResult{owner,
                          lifetime,
                          handler,
                          stale_handler,
                          generation,
                          code,
                          std::move(data)});
        } catch (...) {
        }
    }


    void LvSettingBluetoothPage3::api_result_timer_cb(lv_timer_t *timer) noexcept
{
        auto *self = timer
            ? static_cast<LvSettingBluetoothPage3 *>(lv_timer_get_user_data(timer))
            : nullptr;
        if (!self || timer != self->api_timer_) return;

        std::deque<ApiResult> pending;
        std::deque<AgentPromptRequest> agent_events;
        {
            self->api_tasks_.reap_finished();
            std::lock_guard<std::mutex> lock(self->api_dispatch_->mutex);
            pending.swap(self->api_dispatch_->pending);
            agent_events.swap(self->api_dispatch_->agent_events);
        }

        const std::weak_ptr<bool> lifetime = self->page_lifetime_;
        for (auto &result : pending) {
            if (result.lifetime.expired() || result.owner != self || !self->ComponensObj)
                continue;
            if (result.generation < self->reset_generation_) continue;
            if (result.generation != self->generation_) {
                try {
                    if (result.stale_handler) result.stale_handler(result.code, std::move(result.data));
                } catch (...) {
                }
                if (lifetime.expired()) break;
                continue;
            }

            try {
                if (result.handler)
                    result.handler(result.code, std::move(result.data));
            } catch (...) {
            }
            if (lifetime.expired()) break;
        }
        for (const auto &request : agent_events) {
            if (lifetime.expired() || self->leaving_ || self->reset_pending_ || !self->ComponensObj) {
                if (request.reply) {
                    try { request.reply(false, {}); } catch (...) {}
                }
                continue;
            }
            self->show_agent_prompt(request);
        }
    }


    void LvSettingBluetoothPage3::request_api(std::list<std::string> arguments,
                     ApiHandler handler,
                     ApiHandler stale_handler)
{
        const std::weak_ptr<bool> lifetime = page_lifetime_;
        const uint64_t request_generation = generation_;
        const auto dispatch = api_dispatch_;
        const ApiHandler fallback_handler = handler;
        const ApiHandler fallback_stale_handler = stale_handler;
        auto callback = [dispatch,
                         owner = this,
                         lifetime,
                         request_generation,
                         handler = std::move(handler),
                         stale_handler = std::move(stale_handler)](int code,
                                                                     std::string data) mutable {
            enqueue_api_result(dispatch,
                               owner,
                               lifetime,
                               handler,
                               stale_handler,
                               request_generation,
                               code,
                               std::move(data));
        };

        const bool started = api_tasks_.start(
            [arguments = std::move(arguments), callback = std::move(callback),
             dispatch, request_generation]() mutable {
                {
                    std::lock_guard<std::mutex> lock(dispatch->mutex);
                    if (request_generation < dispatch->minimum_generation) return;
                }
                try {
                    cp0_signal_bt_api(std::move(arguments), callback);
                } catch (...) {
                    callback(-1, "bluetooth service unavailable");
                }
            });
        if (!started) {
            enqueue_api_result(dispatch,
                               this,
                               lifetime,
                               fallback_handler,
                               fallback_stale_handler,
                               request_generation,
                               -1,
                               "bluetooth request could not be scheduled");
        }
    }


    bool LvSettingBluetoothPage3::decode_status(const std::string &data,
                              bool &powered,
                              std::string &address)
{
        settings_bluetooth_com::StatusRecord decoded;
        if (!settings_bluetooth_com::decode_status(data, decoded)) return false;
        powered = decoded.powered;
        address = std::move(decoded.address);
        return true;
    }


    std::string LvSettingBluetoothPage3::device_text(const char *value, size_t size)
{
        if (!value || size == 0) return {};
        size_t length = 0;
        while (length < size && value[length]) ++length;
        return std::string(value, length);
    }


    lv_obj_t *LvSettingBluetoothPage3::create_label(lv_obj_t *parent,
                                  const char *text,
                                  int x,
                                  int y,
                                  int width,
                                  uint32_t color,
                                  const lv_font_t *font)
{
        if (!parent) return nullptr;
        lv_obj_t *label = lv_label_create(parent);
        if (!label) return nullptr;
        lv_label_set_text(label, text ? text : "");
        lv_obj_set_pos(label, x, y);
        if (width > 0) {
            lv_obj_set_width(label, width);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        }
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
        return label;
    }


    void LvSettingBluetoothPage3::request_status()
{
        if (status_pending_ || leaving_ || reset_pending_) return;
        status_pending_ = true;
        request_api(settings_bluetooth_com::status_request(), [this](int code, std::string data) {
            status_pending_ = false;
            const bool decoded = code == 0 && decode_status(data, powered_, adapter_address_);
            if (!decoded) {
                ++generation_;
                status_known_ = false;
                powered_ = false;
                adapter_address_.clear();
                devices_.clear();
                clamp_selection();
                error_message_ = "bluetooth service unavailable.";
                stop_scan();
                render();
                if (status_retry_timer_) lv_timer_resume(status_retry_timer_);
                return;
            }

            // A powered-off adapter still has a stable address. An empty
            // address means the adapter/object manager is not ready yet, so
            // keep the page in a retrying state instead of showing a false
            // "power is off" warning.
            if (!powered_ && adapter_address_.empty()) {
                ++generation_;
                status_known_ = false;
                devices_.clear();
                clamp_selection();
                error_message_ = "bluetooth adapter unavailable. retrying...";
                stop_scan();
                render();
                if (status_retry_timer_) lv_timer_resume(status_retry_timer_);
                return;
            }

            status_known_ = true;
            error_message_.clear();
            if (status_retry_timer_) lv_timer_pause(status_retry_timer_);
            if (!powered_) {
                ++generation_;
                devices_.clear();
                clamp_selection();
                stop_scan();
                render();
                show_power_warning();
                return;
            }
            render();
            if (mode_ == LvSettingBluetoothListMode::Scan) {
                settings_bluetooth_com::StatusRecord record;
                ensure_discoverable(settings_bluetooth_com::decode_status(data, record) && record.discoverable);
                stop_scan([this] { start_scan(); });
            } else {
                refresh_devices();
            }
        }, [this](int, std::string) {
            status_pending_ = false;
        });
    }


    void LvSettingBluetoothPage3::ensure_discoverable(bool currently_discoverable)
{
        // Already visible before this page asked: leave it for whoever set it.
        if (currently_discoverable || discoverable_pending_) return;
        discoverable_pending_ = true;
        const auto complete = [this](int code, std::string data) {
            discoverable_pending_ = false;
            if (settings_bluetooth_com::success_without_payload(code, data)) restore_discoverable_ = true;
        };
        request_api(settings_bluetooth_com::discoverable_request(true), complete, complete);
    }


    void LvSettingBluetoothPage3::status_retry_timer_cb(lv_timer_t *timer) noexcept
    {
        auto *self = timer
            ? static_cast<LvSettingBluetoothPage3 *>(lv_timer_get_user_data(timer))
            : nullptr;
        if (!self || timer != self->status_retry_timer_ || self->leaving_ ||
            self->status_known_ || self->status_pending_ || !self->ComponensObj)
            return;
        self->request_status();
    }


    void LvSettingBluetoothPage3::refresh_devices()
{
        if (!status_known_ || !powered_ || action_pending_ || list_pending_ ||
            reset_pending_) return;
        list_pending_ = true;
        loading_ = true;
        render();

        std::list<std::string> request;
        if (!settings_bluetooth_com::list_request(
                false,
                CP0_BT_DEVICE_MAX,
                request)) {
            list_pending_ = false;
            loading_ = false;
            error_message_ = "bluetooth device list unavailable.";
            render();
            return;
        }
        request_api(std::move(request),
                    [this](int code, std::string data) {
                        list_pending_ = false;
                        loading_ = false;
                        if (!status_known_ || !powered_) {
                            devices_.clear();
                            error_message_ = "bluetooth is off. enable power first.";
                        } else {
                            std::vector<settings_bluetooth_com::DeviceRecord> decoded_devices;
                            if (!settings_bluetooth_com::decode_device_list_reply(
                                    code, data, decoded_devices, CP0_BT_DEVICE_MAX)) {
                                devices_.clear();
                                error_message_ = "bluetooth device list unavailable.";
                            } else {
                                if (mode_ == LvSettingBluetoothListMode::Connected) {
                                    decoded_devices.erase(
                                        std::remove_if(
                                            decoded_devices.begin(),
                                            decoded_devices.end(),
                                            [](const settings_bluetooth_com::DeviceRecord &device) {
                                                return !device.connected && !device.paired;
                                            }),
                                        decoded_devices.end());
                                }
                                devices_.clear();
                                devices_.reserve(decoded_devices.size());
                                for (const auto &device : decoded_devices) {
                                    cp0_bt_device_t converted{};
                                    copy_device_record(device, converted);
                                    devices_.push_back(converted);
                                }
                                error_message_.clear();
                            }
                        }
                        clamp_selection();
                        render();
                    });
    }


    void LvSettingBluetoothPage3::start_scan()
{
        if (mode_ != LvSettingBluetoothListMode::Scan || !powered_ || leaving_ ||
            reset_pending_ ||
            scan_start_pending_ || scan_stop_pending_ || discovery_active_)
            return;

        scan_start_pending_ = true;
        scan_stop_required_ = true;
        loading_ = true;
        error_message_.clear();
        render();
        const auto complete_start = [this](int code, std::string data) {
            scan_start_pending_ = false;
            if (leaving_) {
                discovery_active_ = false;
                loading_ = false;
                return;
            }
            if (scan_stop_after_start_) {
                scan_stop_after_start_ = false;
                const auto continuation = std::move(scan_after_stop_);
                scan_after_stop_ = nullptr;
                if (!settings_bluetooth_com::success_without_payload(code, data)) {
                    discovery_active_ = false;
                    loading_ = false;
                    if (continuation) continuation();
                    return;
                }
                discovery_active_ = true;
                stop_scan(continuation);
                return;
            }
            if (!settings_bluetooth_com::success_without_payload(code, data) || !powered_) {
                discovery_active_ = false;
                error_message_ = powered_
                    ? "bluetooth scan unavailable."
                    : "bluetooth is off. enable power first.";
                loading_ = false;
                render();
                return;
            }

            discovery_active_ = true;
            if (!scan_timer_)
                scan_timer_ = lv_timer_create(scan_timer_cb, 1500, this);
            if (!scan_timer_) {
                error_message_ = "bluetooth scan unavailable.";
                stop_scan();
                render();
                return;
            }
            refresh_devices();
        };
        request_api(settings_bluetooth_com::discovery_start_request(),
                    complete_start,
                    complete_start);
    }


    void LvSettingBluetoothPage3::restart_scan()
{
        if (mode_ != LvSettingBluetoothListMode::Scan || action_pending_ ||
            leaving_ || scan_start_pending_ || scan_stop_pending_ || status_pending_ ||
            reset_pending_)
            return;

        ++generation_;
        list_pending_ = false;
        if (scan_timer_) {
            lv_timer_delete(scan_timer_);
            scan_timer_ = nullptr;
        }
        warning_active_ = false;
        status_known_ = false;
        error_message_.clear();
        devices_.clear();
        clamp_selection();
        render();
        request_status();
    }

    void LvSettingBluetoothPage3::restart_bluetooth()
    {
        if (leaving_ || reset_pending_) return;
        reset_pending_ = true;
        reset_generation_ = ++generation_;
        {
            std::lock_guard<std::mutex> lock(api_dispatch_->mutex);
            api_dispatch_->minimum_generation = reset_generation_;
            api_dispatch_->pending.clear();
            // BtReset completes these agent invocations in the backend.
            api_dispatch_->agent_events.clear();
        }
        clear_agent_prompt();
        agent_pairing_device_.clear();
        agent_authorized_device_.clear();
        cancel_action();
        warning_active_ = false;
        status_pending_ = false;
        scan_start_pending_ = false;
        scan_stop_pending_ = false;
        ++scan_stop_request_id_;
        list_pending_ = false;
        if (scan_timer_) {
            lv_timer_delete(scan_timer_);
            scan_timer_ = nullptr;
        }
        if (status_retry_timer_) lv_timer_pause(status_retry_timer_);
        discovery_active_ = false;
        scan_after_stop_ = nullptr;
        scan_stop_after_start_ = false;
        error_message_.clear();
        devices_.clear();
        clamp_selection();
        render();
        request_api({"BtReset"}, [this](int code, std::string data) {
            reset_pending_ = false;
            if (!settings_bluetooth_com::success_without_payload(code, data)) {
                error_message_ = data.empty() ? "bluetooth restart failed." : data;
                render();
                return;
            }
            scan_stop_required_ = false;
            status_known_ = false;
            request_status();
        });
    }

    void LvSettingBluetoothPage3::stop_scan(std::function<void()> after_stop)
{
        if (scan_timer_) {
            lv_timer_delete(scan_timer_);
            scan_timer_ = nullptr;
        }
        const bool needs_stop = discovery_active_ || scan_start_pending_ || scan_stop_required_;
        if (scan_start_pending_) {
            scan_stop_after_start_ = true;
            if (after_stop) {
                auto previous = std::move(scan_after_stop_);
                scan_after_stop_ = [previous = std::move(previous),
                                    after_stop = std::move(after_stop)]() mutable {
                    if (previous) previous();
                    if (after_stop) after_stop();
                };
            }
            discovery_active_ = false;
            loading_ = false;
            return;
        }
        discovery_active_ = false;
        scan_start_pending_ = false;
        loading_ = false;

        if (scan_stop_pending_) {
            if (after_stop) {
                auto previous = std::move(scan_after_stop_);
                scan_after_stop_ = [previous = std::move(previous),
                                    after_stop = std::move(after_stop)]() mutable {
                    if (previous) previous();
                    if (after_stop) after_stop();
                };
            }
            return;
        }

        if (!needs_stop) {
            if (after_stop) after_stop();
            return;
        }

        scan_stop_pending_ = true;
        scan_stop_required_ = true;
        scan_after_stop_ = std::move(after_stop);
        const uint64_t stop_request_id = ++scan_stop_request_id_;
        const auto complete_stop = [this, stop_request_id](int code,
                                                           std::string data) {
            if (!scan_stop_pending_ || stop_request_id != scan_stop_request_id_) return;
            scan_stop_pending_ = false;
            const auto continuation = std::move(scan_after_stop_);
            scan_after_stop_ = nullptr;
            if (leaving_) return;
            if (!settings_bluetooth_com::success_without_payload(code, data)) {
                error_message_ = "bluetooth scan stop failed.";
                if (action_waiting_for_scan_stop_) {
                    action_waiting_for_scan_stop_ = false;
                    cancel_action();
                }
                render();
                return;
            }
            scan_stop_required_ = false;
            if (continuation) continuation();
        };
        request_api(settings_bluetooth_com::discovery_stop_request(),
                    complete_stop,
                    complete_stop);
    }


    void LvSettingBluetoothPage3::scan_timer_cb(lv_timer_t *timer) noexcept
{
        auto *self = timer
            ? static_cast<LvSettingBluetoothPage3 *>(lv_timer_get_user_data(timer))
            : nullptr;
        if (!self || self->scan_timer_ != timer ||
            !self->discovery_active_ || self->action_pending_)
            return;
        self->refresh_devices();
    }


    void LvSettingBluetoothPage3::submit_action(bool connected, bool paired)
{
        if (!action_pending_) return;
        action_waiting_for_scan_stop_ = false;
        if (connected) {
            request_action("BtDisconnect", action_address_);
        } else if (paired) {
            request_action("BtConnect", action_address_);
        } else {
            std::list<std::string> request;
            if (!settings_bluetooth_com::device_request("BtPair", action_address_, request)) {
                finish_action(-1, "");
                return;
            }
            pair_in_flight_ = true;
            request_api(std::move(request), [this](int code, std::string data) {
                pair_in_flight_ = false;
                if (!settings_bluetooth_com::success_without_payload(code, data)) {
                    cleanup_failed_pair(code, std::move(data));
                    return;
                }
                action_message_ = "connecting...";
                render();
                request_action("BtConnect", action_address_);
            }, [this](int, std::string) {
                pair_in_flight_ = false;
                if (!action_pending_) return;
                cancel_action();
                render();
            });
        }
    }


    void LvSettingBluetoothPage3::activate_selected()
{
        if (action_pending_ || reset_pending_ || status_pending_ ||
            scan_start_pending_ || scan_stop_pending_ || !error_message_.empty() || selected_index_ < 0 ||
            selected_index_ >= static_cast<int>(devices_.size()) || !powered_)
            return;

        const cp0_bt_device_t device = devices_[static_cast<size_t>(selected_index_)];
        const std::string address = device_text(device.address, sizeof(device.address));
        if (!settings_bluetooth_com::valid_device_address(address)) {
            error_message_ = "bluetooth device address is invalid.";
            render();
            return;
        }

        // A new explicit operation requires a fresh authorization decision.
        agent_pairing_device_.clear();
        agent_authorized_device_.clear();
        action_pending_ = true;
        action_address_ = address;
        ++generation_;
        list_pending_ = false;
        action_message_ = device.connected ? "disconnecting..."
            : device.paired ? "connecting..." : "pairing...";
        render();

        const bool connected = device.connected != 0;
        const bool paired = device.paired != 0;
        const auto submit = [this, connected, paired] {
            submit_action(connected, paired);
        };
        if (mode_ == LvSettingBluetoothListMode::Scan) {
            action_waiting_for_scan_stop_ = discovery_active_ ||
                scan_start_pending_ || scan_stop_pending_ || scan_stop_required_;
            if (action_waiting_for_scan_stop_)
                stop_scan(submit);
            else {
                stop_scan();
                submit();
            }
        } else {
            submit();
        }
    }


    void LvSettingBluetoothPage3::request_action(const char *command, const std::string &address)
{
        std::list<std::string> request;
        if (!settings_bluetooth_com::device_request(command, address, request)) {
            finish_action(-1, "");
            return;
        }
        request_api(std::move(request), [this](int code, std::string data) {
            finish_action(code, std::move(data));
        }, [this](int, std::string) {
            if (!action_pending_) return;
            cancel_action();
            render();
        });
    }


    void LvSettingBluetoothPage3::cancel_pairing_on_exit(const std::string &address)
{
        std::list<std::string> request;
        if (!settings_bluetooth_com::device_request("BtCancelPairing", address, request))
            return;

        // Keep this completion UI-independent: the page may be destroyed before
        // BlueZ replies, but the cancellation request itself must still reach it.
        request_api(std::move(request), {}, {});
    }


    void LvSettingBluetoothPage3::cleanup_failed_pair(int code, std::string data)
{
        if (!action_pending_ || pair_cleanup_pending_ || action_address_.empty()) {
            finish_action(code, std::move(data));
            return;
        }

        pair_cleanup_pending_ = true;
        action_message_ = "cleaning up pairing...";
        render();
        std::list<std::string> request;
        if (!settings_bluetooth_com::device_request("BtCancelPairing", action_address_, request)) {
            pair_cleanup_pending_ = false;
            finish_action(code, std::move(data));
            return;
        }

        // CancelPairing releases BlueZ's bonding/agent state before removing
        // the temporary device object. Both operations are best effort.
        request_api(std::move(request),
                    [this, code, data](int, std::string) mutable {
                        std::list<std::string> remove;
                        if (!settings_bluetooth_com::device_request("BtRemove", action_address_, remove)) {
                            pair_cleanup_pending_ = false;
                            finish_action(code, data);
                            return;
                        }
                        request_api(std::move(remove),
                                    [this, code, data](int, std::string) mutable {
                                        pair_cleanup_pending_ = false;
                                        finish_action(code, data);
                                    },
                                    [this, code, data](int, std::string) mutable {
                                        pair_cleanup_pending_ = false;
                                        finish_action(code, data);
                                    });
                    },
                    [this, code, data](int, std::string) mutable {
                        std::list<std::string> remove;
                        if (!settings_bluetooth_com::device_request("BtRemove", action_address_, remove)) {
                            pair_cleanup_pending_ = false;
                            finish_action(code, data);
                            return;
                        }
                        request_api(std::move(remove),
                                    [this, code, data](int, std::string) mutable {
                                        pair_cleanup_pending_ = false;
                                        finish_action(code, data);
                                    },
                                    [this, code, data](int, std::string) mutable {
                                        pair_cleanup_pending_ = false;
                                        finish_action(code, data);
                                    });
                    });
    }


    void LvSettingBluetoothPage3::cancel_action()
{
        action_waiting_for_scan_stop_ = false;
        pair_cleanup_pending_ = false;
        pair_in_flight_ = false;
        action_pending_ = false;
        action_message_.clear();
        action_address_.clear();
    }


    void LvSettingBluetoothPage3::finish_action(int code, std::string data)
{
        const bool succeeded = settings_bluetooth_com::success_without_payload(code, data);
        cancel_action();
        if (!succeeded) {
            agent_pairing_device_.clear();
            agent_authorized_device_.clear();
            error_message_ = data.empty() ? "bluetooth action failed."
                                           : "bluetooth action failed: " + data;
            render();
            if (mode_ == LvSettingBluetoothListMode::Scan)
                restart_scan();
            return;
        }

        error_message_.clear();
        if (mode_ == LvSettingBluetoothListMode::Scan) {
            restart_scan();
        } else {
            render();
            refresh_devices();
        }
    }


    void LvSettingBluetoothPage3::remove_selected()
{
        if (reset_pending_ || status_pending_ ||
            scan_start_pending_ || scan_stop_pending_ || !error_message_.empty()) return;
        if (action_pending_ || selected_index_ < 0 ||
            selected_index_ >= static_cast<int>(devices_.size()))
            return;
        const cp0_bt_device_t &device = devices_[static_cast<size_t>(selected_index_)];
        // The scan list is also the recovery path for paired-but-disconnected
        // devices. Unpaired discoveries keep DEL mapped to rescan.
        if (mode_ == LvSettingBluetoothListMode::Scan && !device.paired && !device.connected)
            return;
        const std::string address = device_text(device.address, sizeof(device.address));
        if (!settings_bluetooth_com::valid_device_address(address)) {
            error_message_ = "bluetooth device address is invalid.";
            render();
            return;
        }
        action_pending_ = true;
        ++generation_;
        list_pending_ = false;
        action_message_ = "removing...";
        render();
        if (mode_ == LvSettingBluetoothListMode::Scan) {
            action_waiting_for_scan_stop_ = discovery_active_ ||
                scan_start_pending_ || scan_stop_pending_ || scan_stop_required_;
            const auto submit = [this, address] { request_action("BtRemove", address); };
            if (action_waiting_for_scan_stop_)
                stop_scan(submit);
            else {
                stop_scan();
                submit();
            }
        } else {
            request_action("BtRemove", address);
        }
    }


    void LvSettingBluetoothPage3::clamp_selection()
{
        if (devices_.empty()) {
            selected_index_ = 0;
            return;
        }
        selected_index_ = std::clamp(
            selected_index_, 0, static_cast<int>(devices_.size()) - 1);
    }


    bool LvSettingBluetoothPage3::move_selection(int direction)
{
        if (action_pending_ || devices_.empty() || direction == 0) return false;
        const int next = std::clamp(
            selected_index_ + direction,
            0,
            static_cast<int>(devices_.size()) - 1);
        if (next == selected_index_) return false;
        selected_index_ = next;
        return true;
    }


    void LvSettingBluetoothPage3::render()
{
        if (!ComponensObj) return;
        if (agent_prompt_active_) {
            render_agent_prompt();
            return;
        }
        if (warning_active_) return;
        lv_obj_clean(ComponensObj);

        const cp0::ui::TitleRow header = cp0::ui::title_row(
            ComponensObj,
            mode_ == LvSettingBluetoothListMode::Scan ? "settings/bluetooth/scan" : "settings/bluetooth/devices");
        std::string status;
        if (!status_known_)
            status = "checking...";
        else
            status = powered_ ? "on" : "off";
        if (!adapter_address_.empty()) {
            status += " ";
            status += adapter_address_;
        }
        cp0::ui::set_text(header.status, status.c_str(), cp0::theme::kIdle);

        const int text_w = cp0::theme::kScreenW - 2 * cp0::theme::kPadX;
        if (reset_pending_) {
            create_label(ComponensObj, "restarting bluetooth...", cp0::theme::kPadX,
                         CP0_ENUM_CAST_INT(LayoutMetric::MessageY), text_w,
                         cp0::theme::kAccent, settings_fonts::sans(11));
            return;
        }

        std::vector<cp0::ui::Chip> chips = mode_ == LvSettingBluetoothListMode::Scan
            ? std::vector<cp0::ui::Chip>{{"ent", "act"}, {"r", "scan"}, {"d", "remove"}, {"b", "reset"}, {"esc", "back"}}
            : std::vector<cp0::ui::Chip>{{"ent", "toggle"}, {"d", "remove"}, {"b", "reset"}, {"esc", "back"}};
        if (action_pending_) {
            create_label(ComponensObj,
                         action_message_.c_str(),
                         cp0::theme::kPadX,
                         CP0_ENUM_CAST_INT(LayoutMetric::MessageY),
                         text_w,
                         cp0::theme::kAccent,
                         settings_fonts::sans(11));
            chips = pair_cleanup_pending_
                ? std::vector<cp0::ui::Chip>{{"b", "reset"}}
                : std::vector<cp0::ui::Chip>{{"b", "reset"}, {"esc", "back"}};
        } else {
            std::string message;
            if (!status_known_)
                message = error_message_.empty()
                    ? "checking bluetooth status..."
                    : error_message_;
            else if (!error_message_.empty())
                message = error_message_;
            else if (!powered_)
                message = "bluetooth is off. enable power first.";
            else if (devices_.empty())
                message = mode_ == LvSettingBluetoothListMode::Scan
                    ? (loading_ ? "scanning..." : "no devices found.")
                    : "no paired devices.";

            if (!message.empty()) {
                lv_obj_t *message_label = create_label(ComponensObj,
                             message.c_str(),
                             cp0::theme::kPadX,
                             CP0_ENUM_CAST_INT(LayoutMetric::MessageY),
                             text_w,
                             error_message_.empty() ? cp0::theme::kIdle : cp0::theme::kWarn,
                             settings_fonts::sans(11));
                if (message_label) {
                    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
                    lv_obj_set_height(message_label, 52);
                }
            }
            if (!error_message_.empty() && mode_ == LvSettingBluetoothListMode::Scan &&
                !status_pending_ && !scan_start_pending_ && !scan_stop_pending_) {
                chips = {{"r", "retry"}, {"b", "reset"}, {"esc", "back"}};
            }

            const int count = message.empty() ? static_cast<int>(devices_.size()) : 0;
            const int offset = count <= CP0_ENUM_CAST_INT(LayoutMetric::VisibleRows)
                ? 0
                : std::clamp(selected_index_ - CP0_ENUM_CAST_INT(LayoutMetric::VisibleRows) / 2,
                             0,
                             count - CP0_ENUM_CAST_INT(LayoutMetric::VisibleRows));
            for (int visible = 0;
                 visible < CP0_ENUM_CAST_INT(LayoutMetric::VisibleRows) && offset + visible < count;
                 ++visible) {
                const int index = offset + visible;
                const auto &device = devices_[static_cast<size_t>(index)];
                const int y = (mode_ == LvSettingBluetoothListMode::Scan
                                   ? CP0_ENUM_CAST_INT(LayoutMetric::ScanRowY)
                                   : CP0_ENUM_CAST_INT(LayoutMetric::ConnectedRowY)) + visible * CP0_ENUM_CAST_INT(LayoutMetric::RowH);
                const bool selected = index == selected_index_;
                if (selected) {
                    cp0::ui::box(ComponensObj,
                                 0,
                                 y,
                                 cp0::theme::kScreenW,
                                 CP0_ENUM_CAST_INT(LayoutMetric::RowH),
                                 cp0::theme::kSelect);
                }

                const uint32_t color = device.connected
                    ? cp0::theme::kAccent
                    : selected ? cp0::theme::kText : cp0::theme::kIdle;
                const std::string name = device_text(device.name, sizeof(device.name));
                const std::string address = device_text(device.address, sizeof(device.address));
                create_label(ComponensObj,
                             name.empty() ? address.c_str() : name.c_str(),
                             8,
                             y + 2,
                             216,
                             color,
                             settings_fonts::cjk_sans(11));
                create_label(ComponensObj,
                             address.c_str(),
                             8,
                             y + 14,
                             216,
                             selected ? cp0::theme::kIdle : cp0::theme::kDim,
                             settings_fonts::mono(11));

                std::string state;
                if (device.connected)
                    state = "connected";
                else if (device.paired)
                    state = "paired";
                else
                    state = std::to_string(device.rssi);
                const lv_font_t *state_font = (device.connected || device.paired)
                    ? settings_fonts::sans(11)
                    : settings_fonts::mono(11);
                lv_obj_t *state_label = create_label(
                    ComponensObj,
                    state.c_str(),
                    cp0::theme::kScreenW - 8 - 80,
                    y + 2,
                    80,
                    color,
                    state_font);
                if (state_label)
                    lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
            }
        }

        cp0::ui::set_footer(cp0::ui::footer(ComponensObj), chips);
    }

    void LvSettingBluetoothPage3::show_agent_prompt(const AgentPromptRequest &request)
    {
        // A new numeric-comparison request starts a fresh pairing session;
        // never carry an earlier connection approval into that session.
        if (request.method == "RequestConfirmation") {
            agent_pairing_device_.clear();
            agent_authorized_device_.clear();
        }

        // BlueZ may ask for authorization once per profile after the user has
        // already approved the connection. Keep the first user decision, then
        // acknowledge subsequent requests for that same device in this
        // session so identical dialogs are not shown repeatedly.
        const std::string request_device = request.device.empty() ? action_address_ : request.device;
        if (is_agent_authorization_method(request.method) &&
            !agent_authorized_device_.empty() &&
            request_device == agent_authorized_device_) {
            if (request.reply) {
                try { request.reply(true, {}); } catch (...) {}
            }
            return;
        }
        const bool supported_method = request.method == "RequestPinCode" ||
                                      request.method == "RequestPasskey" ||
                                      request.method == "RequestConfirmation" ||
                                      request.method == "RequestAuthorization" ||
                                      request.method == "AuthorizeService";
        // Agent requests can be initiated by a remote device. They are valid
        // while this page is visible even when the user did not press Pair.
        if (!supported_method || leaving_ || reset_pending_ || !ComponensObj) {
            if (request.reply) {
                try { request.reply(false, {}); } catch (...) {}
            }
            return;
        }
        if (agent_prompt_active_ && agent_request_.reply) {
            try { agent_request_.reply(false, {}); } catch (...) {}
        }
        agent_request_ = request;
        agent_input_.clear();
        agent_error_.clear();
        agent_prompt_active_ = true;
        enter_text_input_mode();
        render_agent_prompt();
    }

    void LvSettingBluetoothPage3::enter_text_input_mode()
    {
        if (!keyboard_mode_saved_) {
            previous_input_context_ = cp0_keyboard_get_input_context();
            previous_keypad_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
            keyboard_mode_saved_ = true;
        }
        cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
        cp0_keyboard_set_lvgl_keypad_intercept(1);
    }

    void LvSettingBluetoothPage3::restore_text_input_mode()
    {
        if (!keyboard_mode_saved_) return;
        cp0_keyboard_set_input_context(previous_input_context_);
        cp0_keyboard_set_lvgl_keypad_intercept(previous_keypad_intercept_);
        keyboard_mode_saved_ = false;
    }

    void LvSettingBluetoothPage3::render_agent_prompt()
    {
        if (!ComponensObj) return;
        // Keep the model synchronized before destroying/recreating the dialog.
        // This matters when an error causes a redraw while the user is editing.
        if (agent_input_textarea_)
            agent_input_ = lv_textarea_get_text(agent_input_textarea_)
                ? lv_textarea_get_text(agent_input_textarea_) : "";
        agent_input_textarea_ = nullptr;
        agent_overlay_ = nullptr;
        lv_obj_clean(ComponensObj);
        agent_overlay_ = lv_obj_create(ComponensObj);
        if (!agent_overlay_) {
            reject_agent_prompt();
            return;
        }
        lv_obj_set_size(agent_overlay_, CP0_ENUM_CAST_INT(LayoutMetric::ScreenW), CP0_ENUM_CAST_INT(LayoutMetric::ScreenH));
        lv_obj_set_pos(agent_overlay_, 0, 0);
        lv_obj_set_style_bg_color(agent_overlay_, lv_color_hex(cp0::theme::kBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(agent_overlay_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(agent_overlay_, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(agent_overlay_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(agent_overlay_, 0, LV_PART_MAIN);
        lv_obj_clear_flag(agent_overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(agent_overlay_, LV_OBJ_FLAG_SCROLLABLE);

        const bool confirmation = agent_request_.method == "RequestConfirmation";
        const bool device_authorization = agent_request_.method == "RequestAuthorization";
        const bool service_authorization = agent_request_.method == "AuthorizeService";
        const bool authorization = device_authorization || service_authorization;
        const bool passkey = agent_request_.method == "RequestPasskey";
        const int text_x = cp0::theme::kPadX;
        const int text_w = cp0::theme::kScreenW - 2 * cp0::theme::kPadX;

        const cp0::ui::TitleRow header = cp0::ui::title_row(
            agent_overlay_, authorization ? "settings/bluetooth/allow" : "settings/bluetooth/pair");
        cp0::ui::set_text(header.status,
                          confirmation ? "confirm"
                          : device_authorization ? "connection"
                          : service_authorization ? "service"
                          : passkey ? "passkey" : "pin",
                          cp0::theme::kIdle);

        const char *title = confirmation ? "confirm bluetooth pairing"
                            : device_authorization ? "allow device connection?"
                            : service_authorization ? "allow bluetooth service?"
                            : passkey ? "bluetooth passkey" : "bluetooth pin code";
        create_label(agent_overlay_, title, text_x, 22, text_w, cp0::theme::kText, settings_fonts::sans(11));
        const std::string device = agent_request_.device.empty() ? action_address_ : agent_request_.device;
        create_label(agent_overlay_, device.c_str(), text_x, 36, text_w, cp0::theme::kIdle, settings_fonts::mono(11));
        if (confirmation) {
            create_label(agent_overlay_, "pairing code", text_x, 56, text_w, cp0::theme::kIdle, settings_fonts::sans(11));
            const bool has_code = !agent_request_.hint.empty();
            create_label(agent_overlay_,
                         has_code ? agent_request_.hint.c_str() : "(not provided)",
                         text_x,
                         70,
                         text_w,
                         has_code ? cp0::theme::kText : cp0::theme::kIdle,
                         settings_fonts::mono(has_code ? 22 : 11));
        } else if (authorization) {
            create_label(agent_overlay_,
                         device_authorization ? "allow this device to connect?" : "allow this bluetooth service?",
                         text_x,
                         56,
                         text_w,
                         cp0::theme::kText,
                         settings_fonts::sans(11));
            if (service_authorization) {
                const std::string service = agent_request_.hint.empty()
                    ? "service: (not specified)"
                    : "service: " + agent_request_.hint;
                create_label(agent_overlay_, service.c_str(), text_x, 70, text_w, cp0::theme::kIdle, settings_fonts::mono(11));
            }
        } else {
            agent_input_textarea_ = lv_textarea_create(agent_overlay_);
            if (agent_input_textarea_) {
                lv_obj_set_pos(agent_input_textarea_, text_x, 56);
                lv_obj_set_size(agent_input_textarea_, text_w, 32);
                lv_textarea_set_one_line(agent_input_textarea_, true);
                lv_textarea_set_max_length(agent_input_textarea_, passkey ? 6 : 16);
                lv_textarea_set_accepted_chars(agent_input_textarea_, passkey ? "0123456789" : nullptr);
                lv_textarea_set_text(agent_input_textarea_, agent_input_.c_str());
                lv_textarea_set_cursor_pos(agent_input_textarea_, static_cast<int32_t>(agent_input_.size()));
                lv_obj_set_style_text_font(agent_input_textarea_, settings_fonts::mono(22), LV_PART_MAIN);
                lv_obj_set_style_text_color(agent_input_textarea_, lv_color_hex(cp0::theme::kText), LV_PART_MAIN);
                lv_obj_set_style_bg_color(agent_input_textarea_, lv_color_hex(cp0::theme::kLine), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(agent_input_textarea_, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_border_width(agent_input_textarea_, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(agent_input_textarea_, 0, LV_PART_MAIN);
                lv_obj_set_style_pad_left(agent_input_textarea_, 6, LV_PART_MAIN);
                lv_obj_set_style_pad_right(agent_input_textarea_, 6, LV_PART_MAIN);
                lv_obj_set_style_pad_top(agent_input_textarea_, 4, LV_PART_MAIN);
                lv_obj_set_style_pad_bottom(agent_input_textarea_, 4, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(agent_input_textarea_, LV_OPA_TRANSP, LV_PART_CURSOR);
                lv_obj_set_style_border_color(agent_input_textarea_, lv_color_hex(cp0::theme::kAccent), LV_PART_CURSOR);
                lv_obj_set_style_border_width(agent_input_textarea_, 1, LV_PART_CURSOR);
                lv_obj_set_style_border_side(agent_input_textarea_, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
                lv_obj_set_style_pad_left(agent_input_textarea_, -1, LV_PART_CURSOR);
                lv_obj_add_state(agent_input_textarea_, LV_STATE_FOCUSED);
            }
            if (!agent_input_textarea_) {
                reject_agent_prompt();
                return;
            }
        }
        if (!agent_error_.empty())
            create_label(agent_overlay_, agent_error_.c_str(), text_x, 96, text_w, cp0::theme::kWarn, settings_fonts::sans(11));
        const std::vector<cp0::ui::Chip> chips = confirmation
            ? std::vector<cp0::ui::Chip>{{"ent", "confirm"}, {"esc", "reject"}}
            : authorization ? std::vector<cp0::ui::Chip>{{"ent", "allow"}, {"esc", "reject"}}
            : std::vector<cp0::ui::Chip>{{"ent", "ok"}, {"esc", "cancel"}};
        cp0::ui::set_footer(cp0::ui::footer(agent_overlay_), chips);
    }

    void LvSettingBluetoothPage3::clear_agent_prompt()
    {
        agent_prompt_active_ = false;
        agent_overlay_ = nullptr;
        agent_input_textarea_ = nullptr;
        agent_input_.clear();
        agent_error_.clear();
        agent_request_ = {};
        restore_text_input_mode();
        render();
    }

    void LvSettingBluetoothPage3::reject_agent_prompt()
    {
        const auto reply = agent_request_.reply;
        clear_agent_prompt();
        if (reply) {
            try { reply(false, {}); } catch (...) {}
        }
    }

    void LvSettingBluetoothPage3::submit_agent_reply(bool accepted)
    {
        if (!agent_prompt_active_) return;
        const auto request = agent_request_;
        const bool needs_input = request.method == "RequestPinCode" ||
                                 request.method == "RequestPasskey";
        if (needs_input && !agent_input_textarea_) {
            reject_agent_prompt();
            return;
        }
        const char *textarea_text = agent_input_textarea_
            ? lv_textarea_get_text(agent_input_textarea_) : nullptr;
        const std::string text = textarea_text ? textarea_text : "";
        if (accepted) {
            const bool passkey = request.method == "RequestPasskey";
            const bool confirmation = request.method == "RequestConfirmation" ||
                                      request.method == "RequestAuthorization" ||
                                      request.method == "AuthorizeService";
            if (!confirmation && ((passkey && text.size() != 6) ||
                                  (!passkey && (text.empty() || text.size() > 16)))) {
                agent_error_ = passkey ? "passkey must be 6 digits." : "pin must be 1-16 characters.";
                render_agent_prompt();
                return;
            }
            if (!confirmation && passkey && !std::all_of(text.begin(), text.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                })) {
                agent_error_ = "passkey must be 6 digits.";
                render_agent_prompt();
                return;
            }
            const std::string request_device = request.device.empty() ? action_address_ : request.device;
            if (request.method == "RequestConfirmation")
                agent_pairing_device_ = request_device;
            if (is_agent_authorization_method(request.method) &&
                (action_pending_ || request_device == agent_pairing_device_))
                agent_authorized_device_ = request_device;
        }
        clear_agent_prompt();
        if (request.reply) {
            try { request.reply(accepted, accepted ? text : std::string()); } catch (...) {}
        }
    }

    void LvSettingBluetoothPage3::handle_agent_key(const key_item &item)
    {
        if (!agent_prompt_active_ ||
            (item.key_state != KBD_KEY_PRESSED && item.key_state != KBD_KEY_REPEATED)) return;
        const uint32_t key = item.key_code;
        if (key == KEY_ESC) {
            submit_agent_reply(false);
            return;
        }
        if (key == KEY_ENTER || key == KEY_KPENTER) {
            submit_agent_reply(true);
            return;
        }
        // Numeric-comparison requests only need confirmation; do not let
        // unrelated key presses mutate the displayed code.
        if (agent_request_.method == "RequestConfirmation" ||
            agent_request_.method == "RequestAuthorization" ||
            agent_request_.method == "AuthorizeService") return;
        if (key == KEY_BACKSPACE || key == KEY_DELETE) {
            if (!agent_input_textarea_) return;
            lv_textarea_delete_char(agent_input_textarea_);
            if (lv_textarea_get_text(agent_input_textarea_))
                agent_input_ = lv_textarea_get_text(agent_input_textarea_);
            agent_error_.clear();
            return;
        }
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            if (!agent_input_textarea_) return;
            if (key == KEY_LEFT) lv_textarea_cursor_left(agent_input_textarea_);
            else lv_textarea_cursor_right(agent_input_textarea_);
            if (lv_textarea_get_text(agent_input_textarea_))
                agent_input_ = lv_textarea_get_text(agent_input_textarea_);
            return;
        }
        std::string text = launcher_ui::text_input::key_text(key, &item);
        if (text.empty()) return;
        const bool passkey = agent_request_.method == "RequestPasskey";
        if (passkey && (!std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            }) || agent_input_.size() + text.size() > 6)) return;
        if (!passkey && agent_input_.size() + text.size() > 16) return;
        if (!agent_input_textarea_) return;
        lv_textarea_add_text(agent_input_textarea_, text.c_str());
        if (lv_textarea_get_text(agent_input_textarea_))
            agent_input_ = lv_textarea_get_text(agent_input_textarea_);
        agent_error_.clear();
    }

    void LvSettingBluetoothPage3::show_power_warning()
{
        if (!ComponensObj || warning_active_) return;
        warning_active_ = true;
        lv_obj_clean(ComponensObj);

        lv_obj_t *overlay = lv_obj_create(ComponensObj);
        if (!overlay) return;
        lv_obj_set_size(overlay, CP0_ENUM_CAST_INT(LayoutMetric::ScreenW), CP0_ENUM_CAST_INT(LayoutMetric::ScreenH));
        lv_obj_set_pos(overlay, 0, 0);
        lv_obj_set_style_bg_color(overlay, lv_color_hex(cp0::theme::kBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

        const cp0::ui::TitleRow header = cp0::ui::title_row(
            overlay,
            mode_ == LvSettingBluetoothListMode::Scan ? "settings/bluetooth/scan" : "settings/bluetooth/devices");
        cp0::ui::set_text(header.status, "off", cp0::theme::kIdle);
        const int text_w = cp0::theme::kScreenW - 2 * cp0::theme::kPadX;
        create_label(overlay, "bluetooth power is off", cp0::theme::kPadX, 24, text_w,
                     cp0::theme::kWarn, settings_fonts::sans(11));
        create_label(overlay, "turn on power before continuing.", cp0::theme::kPadX, 40, text_w,
                     cp0::theme::kText, settings_fonts::sans(11));
        cp0::ui::set_footer(cp0::ui::footer(overlay), {{"ent", "ok"}, {"esc", "back"}});
    }

    void LvSettingBluetoothPage3::keyboard_event_cb(lv_event_t *event)
{
        if (!event) return;
        auto *self = static_cast<LvSettingBluetoothPage3 *>(lv_event_get_user_data(event));
        auto *item = static_cast<const key_item *>(lv_event_get_param(event));
        if (!self || !item)
            return;
        if (item->key_code == KEY_B || item->semantic_key == KEY_B) {
            if (item->key_state == KBD_KEY_PRESSED) self->restart_bluetooth();
            lv_event_stop_processing(event);
            return;
        }
        if (self->agent_prompt_active_) {
            self->handle_agent_key(*item);
            lv_event_stop_processing(event);
            return;
        }
        const bool pressed = item->key_state == KBD_KEY_PRESSED ||
                             item->key_state == KBD_KEY_REPEATED;
        const bool restart_key = self->mode_ == LvSettingBluetoothListMode::Scan &&
                                 (item->key_code == KEY_R || item->semantic_key == KEY_R);
        if (restart_key) {
            // KEY_R (19) collides with LV_KEY_RIGHT (19). The raw callback
            // owns the restart action; consume all matching LVGL key states,
            // including release, so the same physical key cannot activate a
            // selected device.
            self->suppress_next_navigation_key_ = true;
            if (item->key_state == KBD_KEY_PRESSED)
                self->restart_scan();
            lv_event_stop_processing(event);
            return;
        }
        self->suppress_next_navigation_key_ = false;
        if (pressed && self->mode_ == LvSettingBluetoothListMode::Scan &&
            (item->key_code == KEY_D || item->semantic_key == KEY_D)) {
            self->remove_selected();
        } else if (pressed && self->mode_ == LvSettingBluetoothListMode::Connected &&
            (item->key_code == KEY_D || item->semantic_key == KEY_D)) {
            self->remove_selected();
        }
        lv_event_stop_processing(event);
    }


    void LvSettingBluetoothPage3::handle_key_event(lv_event_t *event)
{
        if (!event || lv_event_get_code(event) != LV_EVENT_KEY) return;
        if (agent_prompt_active_) {
            lv_event_stop_processing(event);
            return;
        }
        const uint32_t key = lv_event_get_key(event);
        // The raw keyboard callback marks the keypad event generated for the
        // same physical R. Check this before navigation handling so R cannot
        // be mistaken for LV_KEY_RIGHT (both are value 19).
        const bool suppress_navigation = suppress_next_navigation_key_;
        suppress_next_navigation_key_ = false;
        if (suppress_navigation && key == LV_KEY_RIGHT) {
            lv_event_stop_processing(event);
            return;
        }
        if (reset_pending_) {
            lv_event_stop_processing(event);
            return;
        }
        if (warning_active_) {
            if (key == LV_KEY_ESC || key == LV_KEY_LEFT || key == LV_KEY_ENTER ||
                key == LV_KEY_RIGHT) {
                warning_active_ = false;
                if (LeaveSelfPage) LeaveSelfPage();
            }
            lv_event_stop_processing(event);
            return;
        }
        if (key == LV_KEY_ESC || key == LV_KEY_LEFT) {
            // Do not tear down the page while the Pair-failure cleanup chain is
            // waiting to remove the temporary BlueZ device object.
            if (pair_cleanup_pending_) {
                lv_event_stop_processing(event);
                return;
            }

            const bool cancel_pair = action_pending_ && pair_in_flight_;
            const std::string pairing_address = cancel_pair ? action_address_ : std::string();
            leaving_ = true;
            ++generation_;
            cancel_action();
            stop_scan();
            // An in-flight discovery stop may own a continuation that would
            // otherwise submit Pair after the user has already left the page.
            scan_after_stop_ = nullptr;
            scan_stop_after_start_ = false;
            if (cancel_pair)
                cancel_pairing_on_exit(pairing_address);
            if (LeaveSelfPage) LeaveSelfPage();
        } else if (key == LV_KEY_UP) {
            if (move_selection(-1)) render();
        } else if (key == LV_KEY_DOWN) {
            if (move_selection(1)) render();
        } else if (key == LV_KEY_ENTER || key == LV_KEY_RIGHT) {
            activate_selected();
        } else if (key == LV_KEY_DEL || key == LV_KEY_NEXT) {
            if (mode_ == LvSettingBluetoothListMode::Scan && selected_index_ >= 0 &&
                selected_index_ < static_cast<int>(devices_.size()) &&
                (devices_[static_cast<size_t>(selected_index_)].paired ||
                 devices_[static_cast<size_t>(selected_index_)].connected))
                remove_selected();
            else if (mode_ == LvSettingBluetoothListMode::Scan)
                restart_scan();
            else
                remove_selected();
        } else if (key == 'r' || key == 'R') {
            if (mode_ == LvSettingBluetoothListMode::Scan) restart_scan();
        } else if (key == 'd' || key == 'D') {
            if (mode_ == LvSettingBluetoothListMode::Connected) remove_selected();
        }
        lv_event_stop_processing(event);
    }


LvSettingBluetoothAliasPage3::LvSettingBluetoothAliasPage3()
    : api_dispatch_(std::make_shared<LvSettingBluetoothAliasPage3::ApiDispatchState>())
{}


    LvSettingBluetoothAliasPage3::LvSettingBluetoothAliasPage3(lv_obj_t *parent,
                                 const NodeIter &parent_node,
                                 std::function<void()> back_callback,
                                 std::string initial_alias,
                                 std::function<void(std::string)> saved_callback,
                                 DeviceNameWriter device_name_writer)
        : parent_node_(parent_node),
          device_name_writer_(std::move(device_name_writer)),
          alias_(std::move(initial_alias)),
          saved_callback_(std::move(saved_callback)),
          api_dispatch_(std::make_shared<LvSettingBluetoothAliasPage3::ApiDispatchState>())
{
        LeaveSelfPage = std::move(back_callback);
        if (device_name_mode()) {
            if (!valid_hostname(alias_)) alias_.clear();
        } else if (!settings_bluetooth_com::valid_alias(alias_)) {
            alias_ = "CardputerZero";
        }
        backend_alias_ = alias_;
        cursor_ = alias_.size();
        create_ui(parent);
    }


LvSettingBluetoothAliasPage3::~LvSettingBluetoothAliasPage3()
{
        restore_text_input_mode();
        if (keyboard_root_ && keyboard_event_dsc_) {
            lv_obj_remove_event_dsc(keyboard_root_, keyboard_event_dsc_);
            keyboard_event_dsc_ = nullptr;
        }
        if (api_timer_) {
            lv_timer_delete(api_timer_);
            api_timer_ = nullptr;
        }
        if (status_retry_timer_) {
            lv_timer_delete(status_retry_timer_);
            status_retry_timer_ = nullptr;
        }
        ++generation_;
        {
            std::lock_guard<std::mutex> lock(api_dispatch_->mutex);
            api_dispatch_->stopped = true;
            api_dispatch_->pending.clear();
        }
        lifetime_.reset();
        api_tasks_.join_all();
        if (ComponensObj) {
            lv_obj_delete(ComponensObj);
            ComponensObj = nullptr;
        }
    }


    void LvSettingBluetoothAliasPage3::AnimateNextIn(std::function<void()> animate_over_func)
{
        if (animate_over_func) animate_over_func();
    }

    void LvSettingBluetoothAliasPage3::AnimateNextOut(std::function<void()> animate_over_func)
{
        if (animate_over_func) animate_over_func();
    }

    void LvSettingBluetoothAliasPage3::LoadNextPage()
{}

    void LvSettingBluetoothAliasPage3::LeaveNextPage()
{
        if (LeaveSelfPage) LeaveSelfPage();
    }


    void LvSettingBluetoothAliasPage3::create_ui(lv_obj_t *parent)
{
        if (!parent) return;
        ComponensObj = lv_obj_create(parent);
        if (!ComponensObj) return;
        lv_obj_set_size(ComponensObj, CP0_ENUM_CAST_INT(LayoutMetric::ScreenW), CP0_ENUM_CAST_INT(LayoutMetric::ScreenH));
        lv_obj_set_pos(ComponensObj, 0, 0);
        lv_obj_set_style_bg_color(ComponensObj, lv_color_hex(cp0::theme::kBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ComponensObj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(ComponensObj, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ComponensObj, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(ComponensObj, 0, LV_PART_MAIN);
        lv_obj_remove_flag(ComponensObj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(ComponensObj, LV_OBJ_FLAG_SCROLLABLE);
        enter_text_input_mode();
        DComponens::lvgl_bind_event(
            ComponensObj,
            LV_EVENT_KEY,
            nullptr,
            std::bind(&LvSettingBluetoothAliasPage3::handle_key_event,
                      this,
                      std::placeholders::_1));

        keyboard_root_ = lv_screen_active();
        if (keyboard_root_ && LV_EVENT_KEYBOARD != 0) {
            keyboard_event_dsc_ = lv_obj_add_event_cb(
                keyboard_root_,
                keyboard_event_cb,
                static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD),
                this);
        }
        api_timer_ = lv_timer_create(api_result_timer_cb, 50, this);
        // BlueZ may still be starting when the editor opens. Retry unresolved
        // status requests, while a powered-off adapter is handled by the
        // power warning below.
        status_retry_timer_ = lv_timer_create(status_retry_timer_cb, 1000, this);
        if (status_retry_timer_) lv_timer_pause(status_retry_timer_);
        render();
        // The device name editor writes the Bluetooth name best-effort after
        // the hostname, so it does not wait on the adapter state.
        if (!device_name_mode()) request_status();
    }


    void LvSettingBluetoothAliasPage3::request_status()
{
        if (status_pending_ || warning_active_ || !ComponensObj) return;
        status_pending_ = true;
        request_api(settings_bluetooth_com::status_request(), [this](int code, std::string data) {
            status_pending_ = false;
            settings_bluetooth_com::StatusRecord status;
            status_known_ = code == 0 && settings_bluetooth_com::decode_status(data, status);
            if (!status_known_) {
                powered_ = false;
                error_message_ = "bluetooth service unavailable.";
                render();
                if (status_retry_timer_) lv_timer_resume(status_retry_timer_);
                return;
            }

            // A powered-off adapter still reports its stable address. An empty
            // address means BlueZ/object-manager is not ready yet, so avoid a
            // false power warning and keep retrying until the adapter appears.
            if (!status.powered && status.address.empty()) {
                status_known_ = false;
                powered_ = false;
                error_message_ = "bluetooth adapter unavailable. retrying...";
                render();
                if (status_retry_timer_) lv_timer_resume(status_retry_timer_);
                return;
            }

            powered_ = status.powered;
            if (status_retry_timer_) lv_timer_pause(status_retry_timer_);
            if (settings_bluetooth_com::valid_alias(status.alias)) {
                backend_alias_ = status.alias;
                if (!alias_edited_) {
                    alias_ = status.alias;
                    cursor_ = alias_.size();
                    if (saved_callback_) saved_callback_(status.alias);
                }
            }
            if (!powered_) {
                show_power_warning();
                return;
            }
            error_message_.clear();
            render();
        }, [this](int, std::string) {
            status_pending_ = false;
            if (status_retry_timer_) lv_timer_resume(status_retry_timer_);
        });
    }


    void LvSettingBluetoothAliasPage3::status_retry_timer_cb(lv_timer_t *timer) noexcept
{
        auto *self = timer
            ? static_cast<LvSettingBluetoothAliasPage3 *>(lv_timer_get_user_data(timer))
            : nullptr;
        if (!self || timer != self->status_retry_timer_ || self->warning_active_ ||
            self->status_known_ || self->status_pending_ || !self->ComponensObj)
            return;
        self->request_status();
    }


    lv_obj_t *LvSettingBluetoothAliasPage3::create_label(lv_obj_t *parent,
                                  const char *text,
                                  int x,
                                  int y,
                                  int width,
                                  uint32_t color,
                                  const lv_font_t *font)
{
        if (!parent) return nullptr;
        lv_obj_t *label = lv_label_create(parent);
        if (!label) return nullptr;
        lv_label_set_text(label, text ? text : "");
        lv_obj_set_pos(label, x, y);
        if (width > 0) {
            lv_obj_set_width(label, width);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        }
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
        return label;
    }


    const lv_font_t *LvSettingBluetoothAliasPage3::input_font(uint16_t size)
{
        return settings_fonts::cjk_sans(size);
    }


    std::size_t LvSettingBluetoothAliasPage3::previous_utf8_start(const std::string &value, std::size_t cursor)
{
        if (cursor == 0 || cursor > value.size()) return 0;
        std::size_t start = cursor - 1;
        while (start > 0 &&
               (static_cast<unsigned char>(value[start]) & 0xC0u) == 0x80u)
            --start;
        return start;
    }


    std::size_t LvSettingBluetoothAliasPage3::next_utf8_end(const std::string &value, std::size_t cursor)
{
        if (cursor >= value.size()) return value.size();
        std::size_t end = cursor + 1;
        while (end < value.size() &&
               (static_cast<unsigned char>(value[end]) & 0xC0u) == 0x80u)
            ++end;
        return end;
    }


    std::size_t LvSettingBluetoothAliasPage3::byte_cursor_from_char_pos(
        const std::string &value, std::size_t char_pos)
    {
        std::size_t bytes = 0;
        std::size_t chars = 0;
        while (bytes < value.size() && chars < char_pos) {
            bytes = next_utf8_end(value, bytes);
            ++chars;
        }
        return bytes;
    }


    void LvSettingBluetoothAliasPage3::render()
{
        if (!ComponensObj) return;
        if (warning_active_) return;
        lv_obj_clean(ComponensObj);
        cp0::ui::title_row(ComponensObj,
                           device_name_mode() ? "settings/system/device name" : "settings/bluetooth/alias");
        create_label(ComponensObj,
                     device_name_mode() ? "hostname and bluetooth name" : "name",
                     CP0_ENUM_CAST_INT(LayoutMetric::AliasLabelX),
                     CP0_ENUM_CAST_INT(LayoutMetric::AliasLabelY),
                     CP0_ENUM_CAST_INT(LayoutMetric::AliasLabelWidth),
                     cp0::theme::kIdle,
                     settings_fonts::sans(11));

        alias_input_ = lv_textarea_create(ComponensObj);
        if (alias_input_) {
            // Start from a neutral textarea style. The default theme gives
            // LV_PART_CURSOR its own text color and padding, which can make
            // the cursor render black and taller than the input box.
            lv_obj_remove_style_all(alias_input_);
            lv_obj_set_pos(alias_input_, CP0_ENUM_CAST_INT(LayoutMetric::AliasTextX), CP0_ENUM_CAST_INT(LayoutMetric::AliasInputY));
            lv_obj_set_size(alias_input_, CP0_ENUM_CAST_INT(LayoutMetric::ScreenW) - CP0_ENUM_CAST_INT(LayoutMetric::AliasTextX) -
                                           CP0_ENUM_CAST_INT(LayoutMetric::AliasTextRightInset),
                            CP0_ENUM_CAST_INT(LayoutMetric::AliasInputH));
            lv_textarea_set_one_line(alias_input_, true);
            lv_textarea_set_max_length(alias_input_, CP0_ENUM_CAST_INT(LayoutMetric::MaxAliasBytes));
            lv_textarea_set_text(alias_input_, alias_.c_str());
            std::size_t cursor_chars = 0;
            for (std::size_t i = 0; i < cursor_ && i < alias_.size(); ++i) {
                if ((static_cast<unsigned char>(alias_[i]) & 0xC0u) != 0x80u)
                    ++cursor_chars;
            }
            lv_textarea_set_cursor_pos(alias_input_, static_cast<int32_t>(cursor_chars));
            lv_obj_set_style_text_font(alias_input_, input_font(11), LV_PART_MAIN);
            lv_obj_set_style_text_letter_space(alias_input_, CP0_ENUM_CAST_INT(LayoutMetric::AliasInputLetterSpace), LV_PART_MAIN);
            lv_obj_set_style_text_color(alias_input_, lv_color_hex(cp0::theme::kText), LV_PART_MAIN);
            lv_obj_set_style_text_color(alias_input_, lv_color_hex(cp0::theme::kText), LV_PART_CURSOR);
            lv_obj_set_style_bg_color(alias_input_, lv_color_hex(cp0::theme::kLine), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(alias_input_, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(alias_input_, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(alias_input_, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_left(alias_input_, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_right(alias_input_, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_top(alias_input_, 5, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(alias_input_, LV_OPA_TRANSP, LV_PART_CURSOR);
            lv_obj_set_style_border_color(alias_input_, lv_color_hex(cp0::theme::kAccent), LV_PART_CURSOR);
            lv_obj_set_style_border_width(alias_input_, CP0_ENUM_CAST_INT(LayoutMetric::AliasInputCursorWidth), LV_PART_CURSOR);
            lv_obj_set_style_border_side(alias_input_, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
            lv_obj_set_style_pad_left(alias_input_, -1, LV_PART_CURSOR);
            lv_obj_set_style_anim_duration(alias_input_, 400, LV_PART_CURSOR);
            if (!saving_) lv_obj_add_state(alias_input_, LV_STATE_FOCUSED);
            else lv_obj_clear_state(alias_input_, LV_STATE_FOCUSED);
        }

        lv_obj_t *footer = cp0::ui::footer(ComponensObj);
        if (saving_)
            cp0::ui::set_footer_message(footer, device_name_mode() ? "setting device name..." : "setting alias...",
                                        cp0::theme::kIdle);
        else
            cp0::ui::set_footer(footer, {{"ent", "set"}, {"bs", "del"}, {"esc", "cancel"}});
        if (!error_message_.empty())
            create_label(ComponensObj,
                         error_message_.c_str(),
                         cp0::theme::kPadX,
                         CP0_ENUM_CAST_INT(LayoutMetric::AliasErrorY),
                         CP0_ENUM_CAST_INT(LayoutMetric::ScreenW) - 2 * cp0::theme::kPadX,
                         cp0::theme::kDanger,
                         settings_fonts::sans(11));
    }


    void LvSettingBluetoothAliasPage3::show_power_warning()
{
        if (!ComponensObj || warning_active_) return;
        warning_active_ = true;
        lv_obj_clean(ComponensObj);
        lv_obj_t *overlay = lv_obj_create(ComponensObj);
        if (!overlay) return;
        lv_obj_set_size(overlay, CP0_ENUM_CAST_INT(LayoutMetric::ScreenW), CP0_ENUM_CAST_INT(LayoutMetric::ScreenH));
        lv_obj_set_pos(overlay, 0, 0);
        lv_obj_set_style_bg_color(overlay, lv_color_hex(cp0::theme::kBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

        const cp0::ui::TitleRow header = cp0::ui::title_row(overlay, "settings/bluetooth/alias");
        cp0::ui::set_text(header.status, "off", cp0::theme::kIdle);
        const int text_w = cp0::theme::kScreenW - 2 * cp0::theme::kPadX;
        create_label(overlay, "bluetooth power is off", cp0::theme::kPadX, 24, text_w,
                     cp0::theme::kWarn, settings_fonts::sans(11));
        create_label(overlay, "turn on power before continuing.", cp0::theme::kPadX, 40, text_w,
                     cp0::theme::kText, settings_fonts::sans(11));
        cp0::ui::set_footer(cp0::ui::footer(overlay), {{"ent", "ok"}, {"esc", "back"}});
    }


    void LvSettingBluetoothAliasPage3::request_api(std::list<std::string> arguments,
                     ApiHandler handler,
                     ApiHandler stale_handler)
{
        const auto dispatch = api_dispatch_;
        const auto lifetime = lifetime_;
        const uint64_t request_generation = generation_;
        const ApiHandler fallback_handler = handler;
        const ApiHandler fallback_stale_handler = stale_handler;
        auto callback = [dispatch,
                         lifetime,
                         request_generation,
                         handler = std::move(handler),
                         stale_handler = std::move(stale_handler)](
                            int code, std::string data) mutable {
            LvSettingBluetoothAliasPage3::enqueue_api_result(dispatch,
                                     lifetime,
                                     std::move(handler),
                                     std::move(stale_handler),
                                     request_generation,
                                     code,
                                     std::move(data));
        };
        const bool started = api_tasks_.start(
            [arguments = std::move(arguments), callback = std::move(callback)]() mutable {
                try {
                    cp0_signal_bt_api(std::move(arguments), callback);
                } catch (...) {
                    callback(-1, "bluetooth service unavailable");
                }
            });
        if (!started)
            LvSettingBluetoothAliasPage3::enqueue_api_result(dispatch,
                                     lifetime,
                                     fallback_handler,
                                     fallback_stale_handler,
                                     request_generation,
                                     -1,
                                     "bluetooth request could not be scheduled");
    }


    void LvSettingBluetoothAliasPage3::api_result_timer_cb(lv_timer_t *timer) noexcept
{
        auto *self = timer
            ? static_cast<LvSettingBluetoothAliasPage3 *>(lv_timer_get_user_data(timer))
            : nullptr;
        if (!self || timer != self->api_timer_) return;
        self->api_tasks_.reap_finished();
        std::deque<LvSettingBluetoothAliasPage3::ApiDispatchState::Result> pending;
        {
            std::lock_guard<std::mutex> lock(self->api_dispatch_->mutex);
            pending.swap(self->api_dispatch_->pending);
        }
        const std::weak_ptr<bool> lifetime = self->lifetime_;
        for (auto &result : pending) {
            if (result.lifetime.expired()) continue;
            try {
                if (result.generation != self->generation_) {
                    if (result.stale_handler)
                        result.stale_handler(result.code, std::move(result.data));
                } else if (result.handler) {
                    result.handler(result.code, std::move(result.data));
                }
            } catch (...) {
            }
            if (lifetime.expired()) break;
        }
    }



    void LvSettingBluetoothAliasPage3::enter_text_input_mode()
{
        if (!keyboard_mode_saved_) {
            previous_input_context_ = cp0_keyboard_get_input_context();
            previous_keypad_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
            keyboard_mode_saved_ = true;
        }
        cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
        cp0_keyboard_set_lvgl_keypad_intercept(1);
    }


    void LvSettingBluetoothAliasPage3::restore_text_input_mode()
{
        if (!keyboard_mode_saved_) return;
        cp0_keyboard_set_input_context(previous_input_context_);
        cp0_keyboard_set_lvgl_keypad_intercept(previous_keypad_intercept_);
        keyboard_mode_saved_ = false;
    }


    void LvSettingBluetoothAliasPage3::append_text(const char *text)
{
        if (saving_ || !text || !text[0]) return;
        const std::string_view input(text);
        if (device_name_mode() &&
            !std::all_of(input.begin(), input.end(),
                         [](unsigned char c) { return std::isalnum(c) != 0 || c == '-'; }))
            return;
        if (!settings_bluetooth_com::valid_text_field(
                input, CP0_ENUM_CAST_SIZE_T(LayoutMetric::MaxAliasBytes), false))
            return;
        if (alias_input_) {
            sync_textarea_state();
            if (alias_.size() + input.size() >
                CP0_ENUM_CAST_SIZE_T(LayoutMetric::MaxAliasBytes))
                return;
            lv_textarea_add_text(alias_input_, text);
            sync_textarea_state();
            alias_edited_ = true;
            error_message_.clear();
            return;
        }
        if (alias_.size() + input.size() > CP0_ENUM_CAST_SIZE_T(LayoutMetric::MaxAliasBytes))
            return;
        const std::size_t length = input.size();
        alias_.insert(cursor_, text, length);
        cursor_ += length;
        alias_edited_ = true;
        error_message_.clear();
        render();
    }


    void LvSettingBluetoothAliasPage3::sync_textarea_state()
{
        if (!alias_input_) return;
        const char *text = lv_textarea_get_text(alias_input_);
        if (!text) return;
        alias_ = text;
        cursor_ = byte_cursor_from_char_pos(
            alias_, static_cast<std::size_t>(lv_textarea_get_cursor_pos(alias_input_)));
    }


void LvSettingBluetoothAliasPage3::save()
{
        if (saving_) return;
        sync_textarea_state();
        if (device_name_mode()) {
            save_device_name();
            return;
        }
        if (!settings_bluetooth_com::valid_alias(alias_)) {
            error_message_ = "name must be 1-63 utf-8 bytes.";
            render();
            return;
        }
        if (!status_known_ || !powered_) {
            show_power_warning();
            return;
        }
        std::list<std::string> request;
        if (!settings_bluetooth_com::alias_request(alias_, request)) {
            error_message_ = "name contains unsupported characters.";
            render();
            return;
        }
        alias_before_save_ = backend_alias_;
        saving_ = true;
        error_message_.clear();
        render();
        request_api(std::move(request), [this](int code, std::string data) {
            saving_ = false;
            if (!settings_bluetooth_com::success_without_payload(code, data)) {
                alias_ = alias_before_save_;
                alias_edited_ = false;
                cursor_ = std::min(cursor_, alias_.size());
                error_message_ = "set alias failed.";
                render();
                return;
            }
            backend_alias_ = alias_;
            alias_edited_ = false;
            if (saved_callback_) saved_callback_(alias_);
            if (LeaveSelfPage) LeaveSelfPage();
        }, [this](int, std::string) {
            saving_ = false;
            alias_ = alias_before_save_;
            alias_edited_ = false;
            cursor_ = std::min(cursor_, alias_.size());
            error_message_ = "set alias cancelled.";
            render();
        });
    }


void LvSettingBluetoothAliasPage3::save_device_name()
{
        if (!valid_hostname(alias_)) {
            error_message_ = "use 1-63 letters, digits or -, not starting or ending with -.";
            render();
            return;
        }
        saving_ = true;
        error_message_.clear();
        render();
        const std::string name = alias_;
        const auto dispatch = api_dispatch_;
        const auto lifetime = lifetime_;
        const uint64_t request_generation = generation_;
        const ApiHandler alias_step = [this, name](int code, std::string error) {
            if (code != 0) {
                saving_ = false;
                error_message_ = "hostname not set: " + error + ".";
                render();
                return;
            }
            backend_alias_ = name;
            alias_edited_ = false;
            if (saved_callback_) saved_callback_(name);
            std::list<std::string> request;
            if (!settings_bluetooth_com::alias_request(name, request)) {
                saving_ = false;
                if (LeaveSelfPage) LeaveSelfPage();
                return;
            }
            request_api(std::move(request), [this](int alias_code, std::string data) {
                saving_ = false;
                if (!settings_bluetooth_com::success_without_payload(alias_code, data)) {
                    error_message_ = "hostname set. bluetooth name not changed.";
                    render();
                    return;
                }
                if (LeaveSelfPage) LeaveSelfPage();
            });
        };
        const bool started = api_tasks_.start([writer = device_name_writer_, name, dispatch, lifetime,
                                               request_generation, alias_step]() {
            std::string error;
            try {
                error = writer(name);
            } catch (...) {
                error = "failed";
            }
            enqueue_api_result(dispatch, lifetime, alias_step, {}, request_generation, error.empty() ? 0 : -1,
                               std::move(error));
        });
        if (!started) {
            saving_ = false;
            error_message_ = "could not start the hostname change.";
            render();
        }
    }


    void LvSettingBluetoothAliasPage3::handle_key_event(lv_event_t *event)
{
        if (!event || lv_event_get_code(event) != LV_EVENT_KEY) return;
        const uint32_t key = lv_event_get_key(event);
        if (warning_active_) {
            if (key == LV_KEY_ESC || key == LV_KEY_LEFT || key == LV_KEY_ENTER ||
                key == LV_KEY_RIGHT) {
                warning_active_ = false;
                if (LeaveSelfPage) LeaveSelfPage();
            }
            lv_event_stop_processing(event);
            return;
        }
        if (key == LV_KEY_ESC) {
            if (!saving_ && LeaveSelfPage) LeaveSelfPage();
        } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            if (!saving_ && cursor_ > 0) {
                if (alias_input_) {
                    lv_textarea_delete_char(alias_input_);
                    sync_textarea_state();
                } else {
                    const std::size_t start = previous_utf8_start(alias_, cursor_);
                    alias_.erase(start, cursor_ - start);
                    cursor_ = start;
                }
                alias_edited_ = true;
                render();
            }
        } else if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
            /* Up/down have no semantic meaning in a one-line alias editor. */
        } else if (key == LV_KEY_LEFT) {
            if (!saving_) {
                if (alias_input_) {
                    lv_textarea_cursor_left(alias_input_);
                    sync_textarea_state();
                } else {
                    cursor_ = previous_utf8_start(alias_, cursor_);
                }
                render();
            }
        } else if (key == LV_KEY_RIGHT) {
            if (!saving_) {
                if (alias_input_) {
                    lv_textarea_cursor_right(alias_input_);
                    sync_textarea_state();
                } else {
                    cursor_ = next_utf8_end(alias_, cursor_);
                }
                render();
            }
        } else if (key == LV_KEY_ENTER) {
            save();
        }
        lv_event_stop_processing(event);
    }


    void LvSettingBluetoothAliasPage3::keyboard_event_cb(lv_event_t *event)
{
        if (!event) return;
        auto *self = static_cast<LvSettingBluetoothAliasPage3 *>(
            lv_event_get_user_data(event));
        auto *item = static_cast<const key_item *>(lv_event_get_param(event));
        if (!self || !item ||
            (item->key_state != KBD_KEY_PRESSED && item->key_state != KBD_KEY_REPEATED))
            return;
        /* Ignore navigation events queued before this page entered text mode;
         * their event snapshot is deliberately authoritative at this boundary
         * so a stale Enter cannot immediately save and leave the editor. */
        if (item->input_context != KBD_INPUT_CONTEXT_TEXT && item->key_code != 0)
            return;

        if (self->warning_active_) {
            if (item->key_code == KEY_ESC || item->key_code == KEY_ENTER ||
                item->key_code == KEY_KPENTER || item->key_code == KEY_LEFT ||
                item->key_code == KEY_RIGHT) {
                self->warning_active_ = false;
                if (self->LeaveSelfPage) self->LeaveSelfPage();
            }
            lv_event_stop_processing(event);
            return;
        }

        /* In text context F/X/Z/C stay printable.  The FN layer still uses
         * Z/C as the editor's left/right cursor shortcuts, so normalize that
         * combination locally before considering text insertion. */
        uint32_t key = item->key_code;
        if ((item->mods & KBD_MOD_FN) != 0) {
            if (key == KEY_Z) key = KEY_LEFT;
            else if (key == KEY_C) key = KEY_RIGHT;
        }

        if (key == KEY_ESC) {
            if (!self->saving_ && self->LeaveSelfPage) self->LeaveSelfPage();
        } else if (key == KEY_ENTER || key == KEY_KPENTER) {
            self->save();
        } else if (key == KEY_BACKSPACE || key == KEY_DELETE) {
            if (!self->saving_ && self->cursor_ > 0) {
                if (self->alias_input_) {
                    lv_textarea_delete_char(self->alias_input_);
                    self->sync_textarea_state();
                } else {
                    const std::size_t start = previous_utf8_start(self->alias_, self->cursor_);
                    self->alias_.erase(start, self->cursor_ - start);
                    self->cursor_ = start;
                }
                self->alias_edited_ = true;
                self->render();
            }
        } else if (key == KEY_LEFT) {
            if (!self->saving_) {
                if (self->alias_input_) {
                    lv_textarea_cursor_left(self->alias_input_);
                    self->sync_textarea_state();
                } else {
                    self->cursor_ = previous_utf8_start(self->alias_, self->cursor_);
                }
                self->render();
            }
        } else if (key == KEY_RIGHT) {
            if (!self->saving_) {
                if (self->alias_input_) {
                    lv_textarea_cursor_right(self->alias_input_);
                    self->sync_textarea_state();
                } else {
                    self->cursor_ = next_utf8_end(self->alias_, self->cursor_);
                }
                self->render();
            }
        } else if (item->utf8[0]) {
            self->append_text(item->utf8);
        }
        lv_event_stop_processing(event);
    }
