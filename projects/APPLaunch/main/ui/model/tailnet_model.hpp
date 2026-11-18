/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Pure helpers for the Tailnet page: a small JSON reader, extraction of the
// nodes from `tailscale status --json`, sorting, ping parsing and row
// formatting. Free of LVGL and process access so they can be unit-tested.
namespace tailnet {

// A parsed JSON value. Object members keep document order: keys[i] names items[i].
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<std::string> keys;
    std::vector<Json> items;

    // Member of an object, or nullptr when this is not an object or has no such key.
    const Json *find(const std::string &key) const;
    // Typed member reads that fall back when the member is missing or has another type.
    std::string string_at(const std::string &key, const std::string &fallback = {}) const;
    bool bool_at(const std::string &key, bool fallback = false) const;
};

// Parses one JSON document. Returns false and sets error on malformed input,
// trailing content, or nesting deeper than kMaxDepth.
constexpr int kMaxDepth = 64;
bool parse_json(const std::string &text, Json &out, std::string &error);

struct Node {
    std::string host;      // HostName, else the first DNSName label, else the address
    std::string dns;       // DNSName without the trailing dot
    std::string ip;        // first IPv4 in TailscaleIPs; empty when there is none
    std::string os;
    std::string relay;     // home relay region, e.g. "lhr"
    std::string cur_addr;  // non-empty while a direct path is in use
    std::string last_seen;
    bool self = false;
    bool online = false;
    bool exit_node = false;
    bool exit_option = false;
};

struct Status {
    std::string backend_state; // "Running", "Stopped", "NeedsLogin", ...
    std::vector<Node> nodes;   // Self first when present, then peers in document order
};

// Every field is optional: a missing or differently typed field leaves the
// default. Returns false only when the text is not a JSON object.
bool parse_status(const std::string &json, Status &out, std::string &error);

// True for dotted-quad IPv4 with each part 0..255 and nothing else. Addresses
// go into shell commands, so nothing else is ever accepted as one.
bool is_ipv4(const std::string &text);

bool running(const Status &status);
// Lowercase title-row text for a backend state: "connected", "stopped", "needs login".
std::string state_label(const std::string &backend_state);

// Self first, then online nodes by name, then offline nodes by name.
void sort_nodes(std::vector<Node> &nodes);

struct Ping {
    bool ok = false;
    int ms = 0;
    bool relay = false; // answered through a DERP relay
    std::string error;  // first output line when no reply was found
};

// Reads the first reply line of `tailscale ping` output: "... in 12ms", and
// whether that line mentions DERP.
Ping parse_ping(const std::string &output);

enum class Path { Self, Direct, Relay, Exit, Offline, Idle };
// A ping result, when there is one, overrides what the status document says.
Path path_of(const Node &node, const Ping *ping = nullptr);
const char *path_label(Path path);

// Three columns for the latency cell: " -" self, "--" offline, "n/a" failed ping.
std::string format_ms(const Node &node, const Ping *ping);
// 0..1 for the latency bar; 80 ms and above fills it.
float latency_fraction(int ms);

// "host       100.64.12.7      12" in fixed columns of 10, 15 and 3.
std::string row_text(const Node &node, const Ping *ping);
// Printable ASCII only, clipped to width columns.
std::string clip(const std::string &text, std::size_t width);
// First non-empty line, lowercased and clipped; for showing a tool's error.
std::string first_line(const std::string &text, std::size_t width);

// Shell command lines. The address must satisfy is_ipv4, otherwise the
// command is empty (ping) or clears the exit node (exit_node_cmd with "").
std::string status_cmd();
std::string ping_cmd(const std::string &ip);
std::string exit_node_cmd(const std::string &ip);

} // namespace tailnet
