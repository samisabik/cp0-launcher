/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_bluez_dbus_client.hpp"
#include "cp0_lvgl_log.h"
#include "../cp0_init_once.hpp"
#include "../cp0_bluetooth_api_contract.hpp"

#include "hal_lvgl_bsp.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string encode_status(const cp0_bt_status_t &status)
{
    std::ostringstream output;
    output << status.powered << '\t'
           << cp0::bluetooth::sanitize_wire_field(status.address) << '\t'
           << status.discoverable << '\t'
           << cp0::bluetooth::sanitize_wire_field(status.alias);
    return output.str();
}

std::string encode_devices(const cp0_bt_device_t *devices, int count)
{
    std::ostringstream output;
    for(int i = 0; devices && i < count; ++i)
    {
        output << cp0::bluetooth::sanitize_wire_field(devices[i].address) << '\t'
               << devices[i].rssi << '\t'
               << devices[i].connected << '\t' << devices[i].paired << '\t'
               << devices[i].trusted << '\t'
               << cp0::bluetooth::sanitize_wire_field(devices[i].name) << '\n';
    }
    return output.str();
}

void report(const std::function<void(int, std::string)> &callback, int code, const std::string &data)
{
    cp0::bluetooth::invoke_callback(callback, code, data);
}

template <typename Loader>
cp0::bluetooth::Reply load_devices(int requested_count, Loader loader)
{
    std::vector<cp0_bt_device_t> devices(static_cast<size_t>(requested_count));
    int count = loader(devices.empty() ? nullptr : devices.data(), static_cast<int>(devices.size()));
    return {count, encode_devices(devices.data(), count)};
}

void api_call(std::list<std::string> args, std::function<void(int, std::string)> callback)
{
    std::ostringstream raw_request;
    for (const auto &arg : args)
        raw_request << '[' << arg << ']';
    cp0_logf("bt", "api request=%s", raw_request.str().c_str());
    cp0::bluetooth::Request request;
    if (!cp0::bluetooth::parse_request(args, request)) {
        cp0_log("bt", "api request rejected by parser");
        report(callback, -1, "invalid bt api request");
        return;
    }
    using cp0::bluetooth::Command;
    if (request.command == Command::Status) {
        report(callback, 0, encode_status(cp0_bluez_dbus::status()));
        return;
    }
    if (request.command == Command::List || request.command == Command::ConnectedList) {
        const bool connected_only = request.command == Command::ConnectedList;
        const auto reply = load_devices(request.max_count,
            [connected_only](cp0_bt_device_t *out, int count) {
                return cp0_bluez_dbus::list(out, count, connected_only);
            });
        report(callback, reply.code, reply.data);
        return;
    }
    if (request.command == Command::Scan) {
        cp0_bluez_dbus::start_discovery_async([request, callback](int code, const std::string &message) {
            if (code != 0) {
                report(callback, code, message);
                return;
            }
            const auto reply = load_devices(request.max_count,
                [](cp0_bt_device_t *out, int count) {
                    return cp0_bluez_dbus::list(out, count, false);
                });
            report(callback, reply.code, reply.data);
        });
        return;
    }

    auto completion = [callback](int code, const std::string &message) {
        cp0_logf("bt", "api async completion code=%d message=%s", code, message.c_str());
        report(callback, code, message == "ok" ? std::string() : message);
    };
    if (request.command == Command::Reset)
        cp0_bluez_dbus::reset_async(std::move(completion));
    else if (request.command == Command::Power)
        cp0_bluez_dbus::set_power_async(request.value, std::move(completion));
    else if (request.command == Command::Alias)
        cp0_bluez_dbus::set_alias_async(request.text.c_str(), std::move(completion));
    else if (request.command == Command::Discoverable)
        cp0_bluez_dbus::set_discoverable_async(request.value, std::move(completion));
    else if (request.command == Command::DiscoveryStart)
        cp0_bluez_dbus::start_discovery_async(std::move(completion));
    else if (request.command == Command::DiscoveryStop)
        cp0_bluez_dbus::stop_discovery_async(std::move(completion));
    else if (request.command == Command::Pair)
        cp0_bluez_dbus::pair_async(request.text.c_str(), std::move(completion));
    else if (request.command == Command::CancelPairing)
        cp0_bluez_dbus::cancel_pairing_async(request.text.c_str(), std::move(completion));
    else if (request.command == Command::Connect)
        cp0_bluez_dbus::connect_async(request.text.c_str(), std::move(completion));
    else if (request.command == Command::Disconnect)
        cp0_bluez_dbus::disconnect_async(request.text.c_str(), std::move(completion));
    else
        cp0_bluez_dbus::remove_async(request.text.c_str(), std::move(completion));
}

} // namespace

extern "C" void init_bluetooth(void)
{
    static cp0::InitOnce initialized;
    initialized.run([] {
        cp0_bluez_dbus::initialize();
        // Bridge Agent1 authentication requests to the UI signal. The
        // callback may run on the BlueZ worker thread; UI consumers must
        // enqueue the prompt on the LVGL thread. Replies are marshalled back
        // through the worker's agent_reply() entry point.
        cp0_bluez_dbus::set_agent_listener([](const cp0_bluez_dbus::AgentRequest &request) {
            // The bridge remains installed for the lifetime of the backend,
            // while the Settings page subscribes only when visible. Never
            // leave a BlueZ Agent1 invocation pending when no UI can answer.
            if (cp0_signal_bt_agent.empty()) {
                // Existing bonds may still ask the agent to authorize an
                // audio/profile connection. Allow only those requests for a
                // device BlueZ already reports as paired; pairing credentials
                // and confirmation remain UI-gated.
                const bool authorization = request.method == "RequestAuthorization" ||
                                            request.method == "AuthorizeService";
                const bool accepted = authorization && request.paired;
                cp0_logf("bt", "agent request without UI method=%s device=%s accepted=%d",
                             request.method.c_str(), request.device.c_str(), accepted ? 1 : 0);
                cp0_bluez_dbus::agent_reply(request.id, accepted, {});
                return;
            }
            cp0_signal_bt_agent(request.id,
                                request.method,
                                request.device,
                                request.method == "RequestConfirmation" ? request.passkey : request.uuid,
                                [id = request.id](bool accepted, std::string text) {
                                    cp0_bluez_dbus::agent_reply(id, accepted, text);
                                });
        });
        return static_cast<bool>(cp0_signal_bt_api.append(
            [](std::list<std::string> args,
               std::function<void(int, std::string)> callback) {
                api_call(std::move(args), std::move(callback));
            }));
    });
}
