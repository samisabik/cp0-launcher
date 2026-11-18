// SPDX-License-Identifier: MIT

#include "../main/ui/model/tailnet_model.hpp"

#include <cassert>
#include <cmath>
#include <string>
#include <vector>

using namespace tailnet;

namespace {

// Shaped like `tailscale status --json`: Self plus a Peer map keyed by node key.
const char *kSample = R"JSON({
  "Version": "1.104.0",
  "BackendState": "Running",
  "TailscaleIPs": ["100.64.12.7", "fd7a:115c:a1e0::7"],
  "Self": {
    "ID": "n1", "HostName": "cp0", "DNSName": "cp0.tail1234.ts.net.", "OS": "linux",
    "TailscaleIPs": ["100.64.12.7", "fd7a:115c:a1e0::7"],
    "CurAddr": "", "Relay": "lhr", "PeerRelay": "", "LastSeen": "0001-01-01T00:00:00Z",
    "Online": true, "ExitNode": false, "ExitNodeOption": false, "Active": false,
    "RxBytes": 0, "Capabilities": ["a", "b"]
  },
  "Health": [],
  "Peer": {
    "nodekey:aa": {
      "HostName": "Printer", "DNSName": "printer.tail1234.ts.net.", "OS": "linux",
      "TailscaleIPs": ["100.64.12.21"], "CurAddr": "", "Relay": "lhr",
      "LastSeen": "2026-11-10T08:00:00Z", "Online": false, "ExitNode": false, "ExitNodeOption": false
    },
    "nodekey:bb": {
      "HostName": "vps-fra", "DNSName": "vps-fra.tail1234.ts.net.", "OS": "linux",
      "TailscaleIPs": ["fd7a:115c:a1e0::3", "100.64.20.3"], "CurAddr": "203.0.113.9:41641", "Relay": "fra",
      "Online": true, "ExitNode": true, "ExitNodeOption": true, "RxBytes": 1.5e6
    },
    "nodekey:cc": {
      "HostName": "MacBook", "DNSName": "macbook.tail1234.ts.net.", "OS": "macOS",
      "TailscaleIPs": ["100.64.12.2"], "CurAddr": "192.168.1.5:41641", "Relay": "lhr",
      "Online": true, "ExitNode": false, "ExitNodeOption": false
    },
    "nodekey:dd": {
      "HostName": "", "DNSName": "phone.tail1234.ts.net.", "OS": "iOS",
      "TailscaleIPs": ["100.64.12.5"], "CurAddr": "", "Relay": "lhr",
      "Online": true, "ExitNodeOption": true
    },
    "nodekey:ee": {
      "HostName": 42, "DNSName": null, "TailscaleIPs": "not-an-array", "Online": "yes",
      "CurAddr": 7, "Relay": [], "ExitNode": 1
    },
    "nodekey:ff": "not an object"
  }
})JSON";

void test_json_values()
{
    Json json;
    std::string error;
    assert(parse_json(R"( {"a": [1, -2.5, 3e2, true, false, null, {"b": {"c": "deep"}}], "s": "x"} )", json, error));
    assert(error.empty());
    assert(json.type == Json::Type::Object);
    assert(json.keys.size() == 2 && json.items.size() == 2);
    const Json *a = json.find("a");
    assert(a && a->type == Json::Type::Array && a->items.size() == 7);
    assert(a->items[0].type == Json::Type::Number && a->items[0].number == 1.0);
    assert(a->items[1].number == -2.5);
    assert(a->items[2].number == 300.0);
    assert(a->items[3].type == Json::Type::Bool && a->items[3].boolean);
    assert(a->items[4].type == Json::Type::Bool && !a->items[4].boolean);
    assert(a->items[5].type == Json::Type::Null);
    const Json *b = a->items[6].find("b");
    assert(b && b->string_at("c") == "deep");
    assert(json.string_at("s") == "x");
    assert(json.string_at("missing", "fallback") == "fallback");
    assert(json.string_at("a", "wrong type") == "wrong type");
    assert(json.bool_at("s", true));
    assert(json.find("nope") == nullptr);
    assert(a->find("a") == nullptr);

    assert(parse_json("[]", json, error) && json.type == Json::Type::Array && json.items.empty());
    assert(parse_json("{}", json, error) && json.type == Json::Type::Object && json.items.empty());
    assert(parse_json("  0  ", json, error) && json.number == 0.0);
    assert(parse_json("-0.125e-1", json, error) && std::fabs(json.number + 0.0125) < 1e-12);
    assert(parse_json("\"\"", json, error) && json.type == Json::Type::String && json.text.empty());
}

void test_json_escapes()
{
    Json json;
    std::string error;
    assert(parse_json(R"("a\"b\\c\/d\n\t\r\b\f")", json, error));
    assert(json.text == "a\"b\\c/d\n\t\r\b\f");
    // U+00E9, U+20AC and a surrogate pair for U+1F600.
    assert(parse_json(R"("caf\u00e9 \u20AC \ud83d\ude00")", json, error));
    assert(json.text == "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
    assert(parse_json(R"({"k\u0041": "v"})", json, error));
    assert(json.string_at("kA") == "v");
    // Raw UTF-8 passes through untouched.
    assert(parse_json("\"\xC3\xA9\"", json, error) && json.text == "\xC3\xA9");
}

void test_json_malformed()
{
    const std::vector<std::string> bad = {
        "",          "   ",        "{",          "[",          "{\"a\"}",     "{\"a\":}",   "{\"a\":1,}",
        "[1,]",      "[1 2]",      "{a:1}",      "\"open",     "\"bad\\q\"",  "\"\\u12\"",  "\"\\uZZZZ\"",
        "\"\\ud83d\"", "\"\\ude00\"", "tru",      "nul",        "01",          "1.",         "1e",
        "-",         "+1",         "{} x",       "[1] [2]",    "\"a\nb\"",    "{\"a\":1 \"b\":2}", "]",
    };
    for (const std::string &text : bad) {
        Json json;
        std::string error;
        assert(!parse_json(text, json, error));
        assert(!error.empty());
        assert(json.type == Json::Type::Null);
    }

    // Deep nesting is refused instead of overflowing the stack.
    Json json;
    std::string error;
    assert(!parse_json(std::string(100000, '['), json, error));
    assert(parse_json(std::string(kMaxDepth, '[') + std::string(kMaxDepth, ']'), json, error));
    assert(!parse_json(std::string(kMaxDepth + 2, '[') + std::string(kMaxDepth + 2, ']'), json, error));
}

void test_parse_status()
{
    Status status;
    std::string error;
    assert(parse_status(kSample, status, error));
    assert(status.backend_state == "Running");
    assert(running(status));
    assert(state_label(status.backend_state) == "connected");
    // Self, five object peers; the string-valued peer is skipped.
    assert(status.nodes.size() == 6);

    const Node &self = status.nodes[0];
    assert(self.self && self.host == "cp0" && self.ip == "100.64.12.7");
    assert(self.dns == "cp0.tail1234.ts.net");
    assert(self.online && self.relay == "lhr" && self.cur_addr.empty() && self.os == "linux");

    const Node &printer = status.nodes[1];
    assert(!printer.self && printer.host == "Printer" && !printer.online);
    assert(printer.last_seen == "2026-11-10T08:00:00Z");

    const Node &vps = status.nodes[2];
    // The first IPv4 is taken even when an IPv6 address comes first.
    assert(vps.ip == "100.64.20.3");
    assert(vps.exit_node && vps.exit_option && vps.cur_addr == "203.0.113.9:41641");

    const Node &phone = status.nodes[4];
    // An empty HostName falls back to the first DNS label.
    assert(phone.host == "phone" && phone.exit_option && !phone.exit_node);

    // Every field has the wrong type: defaults, no crash.
    const Node &odd = status.nodes[5];
    assert(odd.host.empty() && odd.ip.empty() && odd.dns.empty());
    assert(!odd.online && !odd.exit_node && odd.cur_addr.empty() && odd.relay.empty());

    assert(parse_status("{}", status, error));
    assert(status.nodes.empty() && status.backend_state.empty() && !running(status));
    assert(state_label("") == "unknown");
    assert(parse_status(R"({"BackendState":"Stopped","Self":null,"Peer":[1,2]})", status, error));
    assert(status.nodes.empty() && state_label(status.backend_state) == "stopped");
    assert(state_label("NeedsLogin") == "needs login");
    // Self is online while the backend runs even if the field is absent.
    assert(parse_status(R"({"BackendState":"Running","Self":{"HostName":"a"}})", status, error));
    assert(status.nodes.size() == 1 && status.nodes[0].online);
    assert(parse_status(R"({"BackendState":"Stopped","Self":{"HostName":"a"}})", status, error));
    assert(!status.nodes[0].online);

    assert(!parse_status("[]", status, error) && !error.empty());
    assert(!parse_status("", status, error));
    assert(!parse_status("{\"Self\": {", status, error));
    assert(status.nodes.empty());
}

void test_ipv4()
{
    assert(is_ipv4("100.64.12.7"));
    assert(is_ipv4("0.0.0.0"));
    assert(is_ipv4("255.255.255.255"));
    assert(!is_ipv4(""));
    assert(!is_ipv4("100.64.12"));
    assert(!is_ipv4("100.64.12.7.1"));
    assert(!is_ipv4("100.64.12.256"));
    assert(!is_ipv4("100.64.12."));
    assert(!is_ipv4(".100.64.12"));
    assert(!is_ipv4("100..12.7"));
    assert(!is_ipv4("1000.1.1.1"));
    assert(!is_ipv4("fd7a:115c:a1e0::7"));
    assert(!is_ipv4("100.64.12.7; reboot"));
    assert(!is_ipv4("100.64.12.7 "));
    assert(!is_ipv4("$(id)"));
}

void test_sort()
{
    Status status;
    std::string error;
    assert(parse_status(kSample, status, error));
    std::vector<Node> nodes = status.nodes;
    // Put self last to show the sort moves it to the front.
    std::swap(nodes.front(), nodes.back());
    sort_nodes(nodes);
    assert(nodes.size() == 6);
    assert(nodes[0].self && nodes[0].host == "cp0");
    // Online by name, case-insensitive: MacBook, phone, vps-fra.
    assert(nodes[1].host == "MacBook");
    assert(nodes[2].host == "phone");
    assert(nodes[3].host == "vps-fra");
    // Offline by name: the nameless odd node sorts before Printer.
    assert(!nodes[4].online && nodes[4].host.empty());
    assert(!nodes[5].online && nodes[5].host == "Printer");
}

void test_ping()
{
    Ping ping = parse_ping("pong from macbook (100.64.12.2) via 192.168.1.5:41641 in 12ms\n");
    assert(ping.ok && ping.ms == 12 && !ping.relay && ping.error.empty());

    ping = parse_ping("pong from phone (100.64.12.5) via DERP(fra) in 38ms\n");
    assert(ping.ok && ping.ms == 38 && ping.relay);

    // The first reply line wins.
    ping = parse_ping("pong from a (100.64.0.1) via DERP(lhr) in 71ms\n"
                      "pong from a (100.64.0.1) via 10.0.0.2:1 in 4ms\n");
    assert(ping.ok && ping.ms == 71 && ping.relay);

    ping = parse_ping("pong from a (100.64.0.1) via 10.0.0.2:1 in 1.25s\n");
    assert(ping.ok && ping.ms == 1250);
    ping = parse_ping("pong from a (100.64.0.1) via 10.0.0.2:1 in 0.4ms");
    assert(ping.ok && ping.ms == 1);
    ping = parse_ping("pong from a (100.64.0.1) via 10.0.0.2:1 in 2.6ms");
    assert(ping.ok && ping.ms == 3);
    // A host with "in" in its name does not confuse the search.
    ping = parse_ping("pong from in in (100.64.0.1) via 10.0.0.2:1 in 9ms");
    assert(ping.ok && ping.ms == 9);

    ping = parse_ping("ping \"100.64.12.21\" timed out\n");
    assert(!ping.ok && ping.error == "ping \"100.64.12.21\" timed out");
    ping = parse_ping("\n\n  Tailscale is stopped.\n");
    assert(!ping.ok && ping.error == "tailscale is stopped.");
    ping = parse_ping("");
    assert(!ping.ok && ping.error == "no reply");
    ping = parse_ping("pong from a in ms\nlogged in 12 times\n in ");
    assert(!ping.ok);
}

void test_path_and_format()
{
    Node node;
    node.host = "MacBook-Pro-of-Someone";
    node.ip = "100.64.12.2";
    node.online = true;
    assert(path_of(node) == Path::Idle);
    node.relay = "lhr";
    assert(path_of(node) == Path::Relay);
    node.cur_addr = "192.168.1.5:41641";
    assert(path_of(node) == Path::Direct);

    Ping relayed;
    relayed.ok = true;
    relayed.ms = 38;
    relayed.relay = true;
    assert(path_of(node, &relayed) == Path::Relay);
    Ping failed;
    assert(path_of(node, &failed) == Path::Direct);

    node.exit_node = true;
    assert(path_of(node, &relayed) == Path::Exit);
    node.online = false;
    assert(path_of(node) == Path::Offline);
    node.self = true;
    assert(path_of(node) == Path::Self);
    assert(std::string(path_label(Path::Self)) == "self");
    assert(std::string(path_label(Path::Direct)) == "direct");
    assert(std::string(path_label(Path::Relay)) == "relay");
    assert(std::string(path_label(Path::Exit)) == "exit");
    assert(std::string(path_label(Path::Offline)) == "offline");
    assert(std::string(path_label(Path::Idle)) == "idle");

    assert(format_ms(node, nullptr) == " -");
    node.self = false;
    assert(format_ms(node, &relayed) == "--");
    node.online = true;
    assert(format_ms(node, nullptr).empty());
    assert(format_ms(node, &relayed) == "38");
    assert(format_ms(node, &failed) == "n/a");
    relayed.ms = 4321;
    assert(format_ms(node, &relayed) == "999");
    relayed.ms = 38;

    //                                    host(10)+1  addr(15)+1      ms(3)
    assert(row_text(node, &relayed) == "macbook-pr 100.64.12.2      38");
    assert(row_text(node, nullptr) == "macbook-pr 100.64.12.2        ");
    node.host = "nas";
    node.ip = "100.100.100.100";
    assert(row_text(node, &failed) == "nas        100.100.100.100 n/a");
    node.ip.clear();
    assert(row_text(node, nullptr).substr(11, 1) == "-");
    assert(row_text(node, nullptr).size() == 30);

    assert(clip("h\xC3\xA9llo\x01", 10) == "h?llo?");
    assert(clip("sam\xE2\x80\x99s mac", 20) == "sam's mac");
    assert(clip("abcdef", 3) == "abc");
    assert(first_line("\n \r\n  Access Denied: needs operator\nmore", 13) == "access denied");
    assert(first_line("", 10).empty());

    assert(latency_fraction(0) == 0.0f);
    assert(latency_fraction(-3) == 0.0f);
    assert(std::fabs(latency_fraction(40) - 0.5f) < 1e-6f);
    assert(latency_fraction(80) == 1.0f);
    assert(latency_fraction(500) == 1.0f);
}

void test_commands()
{
    assert(status_cmd() == "tailscale status --json");
    assert(ping_cmd("100.64.12.2") == "tailscale ping -c 1 100.64.12.2");
    assert(ping_cmd("100.64.12.2; rm -rf /").empty());
    assert(ping_cmd("").empty());
    assert(exit_node_cmd("100.64.20.3") == "tailscale set --exit-node=100.64.20.3");
    assert(exit_node_cmd("") == "tailscale set --exit-node=");
    assert(exit_node_cmd("$(reboot)") == "tailscale set --exit-node=");
}

} // namespace

int main()
{
    test_json_values();
    test_json_escapes();
    test_json_malformed();
    test_parse_status();
    test_ipv4();
    test_sort();
    test_ping();
    test_path_and_format();
    test_commands();
    return 0;
}
