/*
 * SPDX-License-Identifier: MIT
 */

#include "tailnet_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tailnet {

namespace {

class Parser
{
public:
    Parser(const std::string &text, std::string &error) : text_(text), error_(error) {}

    bool document(Json &out)
    {
        if (!value(out, 0)) return false;
        skip_space();
        if (pos_ != text_.size()) return fail("trailing content");
        return true;
    }

private:
    const std::string &text_;
    std::string &error_;
    std::size_t pos_ = 0;

    bool fail(const char *what)
    {
        error_ = std::string(what) + " at offset " + std::to_string(pos_);
        return false;
    }

    void skip_space()
    {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool literal(const char *word)
    {
        const std::size_t length = std::char_traits<char>::length(word);
        if (text_.compare(pos_, length, word) != 0) return fail("unknown literal");
        pos_ += length;
        return true;
    }

    bool value(Json &out, int depth)
    {
        if (depth > kMaxDepth) return fail("nesting too deep");
        skip_space();
        if (pos_ >= text_.size()) return fail("unexpected end");
        out = Json();
        const char c = text_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.type = Json::Type::String;
            return string(out.text);
        }
        if (c == 't') {
            out.type = Json::Type::Bool;
            out.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            out.type = Json::Type::Bool;
            return literal("false");
        }
        if (c == 'n') return literal("null");
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }

    bool object(Json &out, int depth)
    {
        out.type = Json::Type::Object;
        ++pos_;
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected a member name");
            std::string key;
            if (!string(key)) return false;
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            Json member;
            if (!value(member, depth + 1)) return false;
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(member));
            skip_space();
            if (pos_ >= text_.size()) return fail("unterminated object");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Json &out, int depth)
    {
        out.type = Json::Type::Array;
        ++pos_;
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            Json item;
            if (!value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skip_space();
            if (pos_ >= text_.size()) return fail("unterminated array");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(unsigned &out)
    {
        if (pos_ + 4 > text_.size()) return fail("short unicode escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad unicode escape");
        }
        return true;
    }

    static void append_utf8(std::string &out, unsigned cp)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string &out)
    {
        ++pos_;
        out.clear();
        while (true) {
            if (pos_ >= text_.size()) return fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return fail("unterminated escape");
            const char e = text_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // A high surrogate must be followed by an escaped low surrogate.
                    unsigned low = 0;
                    if (text_.compare(pos_, 2, "\\u") != 0) return fail("lone surrogate");
                    pos_ += 2;
                    if (!hex4(low)) return false;
                    if (low < 0xDC00 || low > 0xDFFF) return fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("lone surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default:
                --pos_;
                return fail("unknown escape");
            }
        }
    }

    bool digits()
    {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        return pos_ > start;
    }

    bool number(Json &out)
    {
        const std::size_t start = pos_;
        if (text_[pos_] == '-') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '0') {
            ++pos_;
        } else if (!digits()) {
            return fail("bad number");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (!digits()) return fail("bad fraction");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (!digits()) return fail("bad exponent");
        }
        out.type = Json::Type::Number;
        out.number = std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }
};

std::string lower(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string pad(std::string text, std::size_t width)
{
    if (text.size() < width) text.append(width - text.size(), ' ');
    return text;
}

Node read_node(const Json &json, bool self)
{
    Node node;
    node.self = self;
    node.os = json.string_at("OS");
    node.relay = json.string_at("Relay");
    node.cur_addr = json.string_at("CurAddr");
    node.last_seen = json.string_at("LastSeen");
    node.online = json.bool_at("Online");
    node.exit_node = json.bool_at("ExitNode");
    node.exit_option = json.bool_at("ExitNodeOption");
    node.dns = json.string_at("DNSName");
    if (!node.dns.empty() && node.dns.back() == '.') node.dns.pop_back();
    if (const Json *ips = json.find("TailscaleIPs"); ips && ips->type == Json::Type::Array) {
        for (const Json &ip : ips->items) {
            if (ip.type == Json::Type::String && is_ipv4(ip.text)) {
                node.ip = ip.text;
                break;
            }
        }
    }
    node.host = json.string_at("HostName");
    if (node.host.empty()) node.host = node.dns.substr(0, node.dns.find('.'));
    if (node.host.empty()) node.host = node.ip;
    return node;
}

} // namespace

const Json *Json::find(const std::string &key) const
{
    if (type != Type::Object) return nullptr;
    for (std::size_t i = 0; i < keys.size() && i < items.size(); ++i)
        if (keys[i] == key) return &items[i];
    return nullptr;
}

std::string Json::string_at(const std::string &key, const std::string &fallback) const
{
    const Json *member = find(key);
    return member && member->type == Type::String ? member->text : fallback;
}

bool Json::bool_at(const std::string &key, bool fallback) const
{
    const Json *member = find(key);
    return member && member->type == Type::Bool ? member->boolean : fallback;
}

bool parse_json(const std::string &text, Json &out, std::string &error)
{
    error.clear();
    Parser parser(text, error);
    if (parser.document(out)) return true;
    out = Json();
    return false;
}

bool is_ipv4(const std::string &text)
{
    int parts = 0;
    std::size_t pos = 0;
    while (true) {
        const std::size_t start = pos;
        int value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && pos - start < 3)
            value = value * 10 + (text[pos++] - '0');
        if (pos == start || value > 255) return false;
        ++parts;
        if (pos == text.size()) return parts == 4;
        if (text[pos] != '.' || parts == 4) return false;
        ++pos;
    }
}

bool parse_status(const std::string &json, Status &out, std::string &error)
{
    out = Status();
    Json root;
    if (!parse_json(json, root, error)) return false;
    if (root.type != Json::Type::Object) {
        error = "status is not an object";
        return false;
    }
    out.backend_state = root.string_at("BackendState");
    if (const Json *self = root.find("Self"); self && self->type == Json::Type::Object) {
        Node node = read_node(*self, true);
        // This machine is reachable from itself whenever the backend runs.
        node.online = node.online || out.backend_state == "Running";
        out.nodes.push_back(std::move(node));
    }
    if (const Json *peers = root.find("Peer"); peers && peers->type == Json::Type::Object) {
        for (const Json &peer : peers->items)
            if (peer.type == Json::Type::Object) out.nodes.push_back(read_node(peer, false));
    }
    return true;
}

bool running(const Status &status)
{
    return status.backend_state == "Running";
}

std::string state_label(const std::string &backend_state)
{
    if (backend_state == "Running") return "connected";
    if (backend_state.empty()) return "unknown";
    // "NeedsLogin" -> "needs login"
    std::string out;
    for (std::size_t i = 0; i < backend_state.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(backend_state[i]);
        if (i > 0 && std::isupper(c)) out += ' ';
        out += static_cast<char>(std::tolower(c));
    }
    return out;
}

void sort_nodes(std::vector<Node> &nodes)
{
    auto rank = [](const Node &node) { return node.self ? 0 : node.online ? 1 : 2; };
    std::stable_sort(nodes.begin(), nodes.end(), [&](const Node &a, const Node &b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        const std::string left = lower(a.host);
        const std::string right = lower(b.host);
        if (left != right) return left < right;
        return a.ip < b.ip;
    });
}

Ping parse_ping(const std::string &output)
{
    Ping ping;
    std::size_t start = 0;
    while (start <= output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        const std::string line = output.substr(start, end - start);
        start = end + 1;
        // Look for " in <number>ms" (or "<number>s") anywhere in the line.
        std::size_t at = 0;
        while ((at = line.find(" in ", at)) != std::string::npos) {
            at += 4;
            const char *begin = line.c_str() + at;
            char *stop = nullptr;
            if (!(*begin >= '0' && *begin <= '9')) continue;
            const double value = std::strtod(begin, &stop);
            if (!stop || stop == begin) continue;
            const std::string unit(stop);
            double ms = -1.0;
            if (unit.compare(0, 2, "ms") == 0) ms = value;
            else if (unit.compare(0, 1, "s") == 0) ms = value * 1000.0;
            else if (unit.compare(0, 2, "\xC2\xB5") == 0 || unit.compare(0, 2, "us") == 0) ms = value / 1000.0;
            if (ms < 0.0 || ms > 600000.0) continue;
            ping.ok = true;
            ping.ms = std::max(1, static_cast<int>(std::lround(ms)));
            ping.relay = line.find("DERP") != std::string::npos;
            return ping;
        }
    }
    ping.error = first_line(output, 44);
    if (ping.error.empty()) ping.error = "no reply";
    return ping;
}

Path path_of(const Node &node, const Ping *ping)
{
    if (node.self) return Path::Self;
    if (!node.online) return Path::Offline;
    if (node.exit_node) return Path::Exit;
    if (ping && ping->ok) return ping->relay ? Path::Relay : Path::Direct;
    if (!node.cur_addr.empty()) return Path::Direct;
    if (!node.relay.empty()) return Path::Relay;
    return Path::Idle;
}

const char *path_label(Path path)
{
    switch (path) {
    case Path::Self: return "self";
    case Path::Direct: return "direct";
    case Path::Relay: return "relay";
    case Path::Exit: return "exit";
    case Path::Offline: return "offline";
    case Path::Idle: return "idle";
    }
    return "";
}

std::string format_ms(const Node &node, const Ping *ping)
{
    if (node.self) return " -";
    if (!node.online) return "--";
    if (!ping) return "";
    if (!ping->ok) return "n/a";
    return ping->ms > 999 ? "999" : std::to_string(ping->ms);
}

float latency_fraction(int ms)
{
    if (ms <= 0) return 0.0f;
    return ms >= 80 ? 1.0f : static_cast<float>(ms) / 80.0f;
}

std::string clip(const std::string &text, std::size_t width)
{
    // The chrome font is ASCII: typographic quotes become plain ones and any
    // other non-ASCII character becomes a single '?', whatever its byte length.
    std::string out;
    for (std::size_t i = 0; i < text.size() && out.size() < width;) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
            ++i;
            continue;
        }
        std::size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (i + length > text.size()) length = text.size() - i;
        const std::string sequence = text.substr(i, length);
        if (sequence == "\xE2\x80\x98" || sequence == "\xE2\x80\x99")
            out += '\'';
        else if (sequence == "\xE2\x80\x9C" || sequence == "\xE2\x80\x9D")
            out += '"';
        else
            out += '?';
        i += length;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
    }
    return out;
}

std::string first_line(const std::string &text, std::size_t width)
{
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) return lower(clip(line.substr(first), width));
        start = end + 1;
    }
    return {};
}

std::string row_text(const Node &node, const Ping *ping)
{
    const std::string ms = format_ms(node, ping);
    return pad(lower(clip(node.host, 10)), 11) + pad(clip(node.ip.empty() ? "-" : node.ip, 15), 16) +
           std::string(ms.size() < 3 ? 3 - ms.size() : 0, ' ') + ms;
}

std::string status_cmd()
{
    return "tailscale status --json";
}

std::string ping_cmd(const std::string &ip)
{
    return is_ipv4(ip) ? "tailscale ping -c 1 " + ip : std::string();
}

std::string exit_node_cmd(const std::string &ip)
{
    return "tailscale set --exit-node=" + (is_ipv4(ip) ? ip : std::string());
}

} // namespace tailnet
