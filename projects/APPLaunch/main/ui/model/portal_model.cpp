/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "portal_model.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

namespace portal {

namespace {

std::string clean_ssid(const std::string &ssid)
{
    std::string out;
    for (unsigned char c : ssid) {
        if (out.size() >= 32) break;
        out.push_back(std::iscntrl(c) ? ' ' : static_cast<char>(c));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? std::string(kDefaultSsid) : out;
}

int clamp_channel(int channel)
{
    return channel >= 1 && channel <= 14 ? channel : kDefaultChannel;
}

} // namespace

std::string hostapd_conf(const std::string &ssid, int channel)
{
    std::ostringstream out;
    out << "interface=" << kInterface << "\n"
        << "driver=nl80211\n"
        << "ssid=" << clean_ssid(ssid) << "\n"
        << "hw_mode=g\n"
        << "channel=" << clamp_channel(channel) << "\n"
        << "auth_algs=1\n"
        << "wmm_enabled=0\n"
        << "# open network, no encryption: a test AP, not an uplink\n";
    return out.str();
}

std::string dnsmasq_conf(const std::string &dir)
{
    std::ostringstream out;
    out << "interface=" << kInterface << "\n"
        << "bind-interfaces\n"
        << "except-interface=lo\n"
        << "no-resolv\n"
        << "dhcp-range=10.0.0.10,10.0.0.200,255.255.255.0,12h\n"
        << "dhcp-option=3," << kGatewayIp << "\n"
        << "dhcp-option=6," << kGatewayIp << "\n"
        << "address=/#/" << kGatewayIp << "\n"
        << "dhcp-leasefile=" << dir << "/leases\n"
        << "pid-file=" << dir << "/dnsmasq.pid\n";
    return out.str();
}

std::string index_html()
{
    return "<!doctype html>\n"
           "<meta name=viewport content=\"width=device-width,initial-scale=1\">\n"
           "<title>Portal</title>\n"
           "<style>body{font-family:system-ui;max-width:380px;margin:3rem auto;padding:1rem;text-align:center}</style>\n"
           "<h1>Hello, world</h1>\n"
           "<p>You are connected to a cp0 test access point.</p>\n";
}

std::string serve_py(const std::string &dir)
{
    std::ostringstream out;
    out << "import http.server, socketserver\n"
        << "from datetime import datetime, timezone\n"
        << "HTML = open(\"" << dir << "/index.html\", \"rb\").read()\n"
        << "LOG = \"" << dir << "/visits.log\"\n"
        << "class H(http.server.BaseHTTPRequestHandler):\n"
        << "    def do_GET(self):\n"
        << "        try:\n"
        << "            ua = self.headers.get(\"User-Agent\", \"\").replace(\"\\t\", \" \").replace(\"\\n\", \" \")\n"
        << "            with open(LOG, \"a\") as f:\n"
        << "                f.write(\"\\t\".join([datetime.now(timezone.utc).isoformat(timespec=\"seconds\"),\n"
        << "                                    self.client_address[0], self.path, ua]) + \"\\n\")\n"
        << "        except Exception:\n"
        << "            pass\n"
        << "        self.send_response(200)\n"
        << "        self.send_header(\"Content-Type\", \"text/html\")\n"
        << "        self.send_header(\"Content-Length\", str(len(HTML)))\n"
        << "        self.end_headers()\n"
        << "        self.wfile.write(HTML)\n"
        << "    def log_message(self, *a):\n"
        << "        pass\n"
        << "socketserver.TCPServer.allow_reuse_address = True\n"
        << "socketserver.TCPServer((\"0.0.0.0\", " << kPortalPort << "), H).serve_forever()\n";
    return out.str();
}

std::string start_script(const std::string &dir)
{
    std::ostringstream out;
    out << "#!/bin/sh\n"
        << "set -e\n"
        << "IF=" << kInterface << "\n"
        << "DIR=\"" << dir << "\"\n"
        << "rfkill unblock wlan 2>/dev/null || true\n"
        << "nmcli dev set \"$IF\" managed no 2>/dev/null || true\n"
        << "ip addr flush dev \"$IF\" || true\n"
        << "ip addr add " << kGatewayIp << "/24 dev \"$IF\"\n"
        << "ip link set \"$IF\" up\n"
        << "for P in 80 443; do\n"
        << "  iptables -t nat -C PREROUTING -i \"$IF\" -p tcp --dport $P -j DNAT --to " << kGatewayIp << ":"
        << kPortalPort << " 2>/dev/null || \\\n"
        << "  iptables -t nat -A PREROUTING -i \"$IF\" -p tcp --dport $P -j DNAT --to " << kGatewayIp << ":"
        << kPortalPort << "\n"
        << "done\n"
        << "setsid python3 \"$DIR/serve.py\" >\"$DIR/serve.log\" 2>&1 &\n"
        << "echo $! > \"$DIR/serve.pid\"\n"
        << "dnsmasq -C \"$DIR/dnsmasq.conf\"\n"
        << "hostapd -B -P \"$DIR/hostapd.pid\" \"$DIR/hostapd.conf\"\n";
    return out.str();
}

std::string stop_script(const std::string &dir)
{
    std::ostringstream out;
    out << "#!/bin/sh\n"
        << "IF=" << kInterface << "\n"
        << "DIR=\"" << dir << "\"\n"
        << "for F in hostapd dnsmasq serve; do\n"
        << "  [ -f \"$DIR/$F.pid\" ] && kill \"$(cat \"$DIR/$F.pid\")\" 2>/dev/null\n"
        << "  rm -f \"$DIR/$F.pid\"\n"
        << "done\n"
        << "for P in 80 443; do\n"
        << "  iptables -t nat -D PREROUTING -i \"$IF\" -p tcp --dport $P -j DNAT --to " << kGatewayIp << ":"
        << kPortalPort << " 2>/dev/null || true\n"
        << "done\n"
        << "ip addr flush dev \"$IF\" 2>/dev/null || true\n"
        << "ip link set \"$IF\" down 2>/dev/null || true\n"
        << "nmcli dev set \"$IF\" managed yes 2>/dev/null || true\n";
    return out.str();
}

int parse_rc(const std::string &log)
{
    const std::size_t at = log.rfind("#rc ");
    if (at == std::string::npos) return -1;
    return std::atoi(log.c_str() + at + 4);
}

int count_leases(const std::string &leasefile)
{
    int count = 0;
    std::istringstream in(leasefile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.find_first_not_of(" \t\r") != std::string::npos) ++count;
    }
    return count;
}

std::string last_line(const std::string &log)
{
    std::istringstream in(log);
    std::string line, last;
    while (std::getline(in, line)) {
        const std::size_t end = line.find_last_not_of(" \t\r");
        if (end == std::string::npos) continue;
        const std::size_t start = line.find_first_not_of(" \t\r");
        const std::string trimmed = line.substr(start, end - start + 1);
        if (trimmed[0] == '#') continue; // the rc marker, not an error
        last = trimmed;
    }
    return last;
}

} // namespace portal
