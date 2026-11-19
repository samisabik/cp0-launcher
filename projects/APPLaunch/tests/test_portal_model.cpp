/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/portal_model.hpp"

#include <cassert>
#include <string>

using namespace portal;

int main()
{
    // hostapd.conf names the interface and the requested open channel.
    const std::string conf = hostapd_conf("cp0 portal", 6);
    assert(conf.find("interface=wlan1\n") != std::string::npos);
    assert(conf.find("ssid=cp0 portal\n") != std::string::npos);
    assert(conf.find("channel=6\n") != std::string::npos);
    assert(conf.find("wpa") == std::string::npos); // open, never WPA

    // A control character cannot break out of the ssid line, and an empty or
    // out-of-range value falls back to the defaults.
    assert(hostapd_conf("a\nb", 6).find("ssid=a b\n") != std::string::npos);
    assert(hostapd_conf("", 6).find(std::string("ssid=") + kDefaultSsid + "\n") != std::string::npos);
    assert(hostapd_conf("x", 99).find("channel=6\n") != std::string::npos);
    std::string long_ssid(40, 'z');
    assert(hostapd_conf(long_ssid, 1).find("ssid=" + std::string(32, 'z') + "\n") != std::string::npos);

    // dnsmasq serves only wlan1 and points every name at the gateway.
    const std::string dns = dnsmasq_conf("/home/admin/portal");
    assert(dns.find("interface=wlan1\n") != std::string::npos);
    assert(dns.find("bind-interfaces\n") != std::string::npos);
    assert(dns.find("address=/#/10.0.0.1\n") != std::string::npos);
    assert(dns.find("dhcp-leasefile=/home/admin/portal/leases\n") != std::string::npos);

    // The served page has no form and nothing is logged.
    const std::string html = index_html();
    assert(html.find("Hello, world") != std::string::npos);
    assert(html.find("<form") == std::string::npos);
    assert(html.find("password") == std::string::npos);
    assert(serve_py("/d").find("/d/index.html") != std::string::npos);

    // Scripts act on wlan1 only and the stop script undoes the start.
    const std::string start = start_script("/d");
    assert(start.find("IF=wlan1\n") != std::string::npos);
    // Re-enumerates the USB adapter so its AP receive path works after boot.
    assert(start.find("/authorized") != std::string::npos);
    assert(start.find("-A PREROUTING -i \"$IF\" -p tcp --dport $P -j DNAT --to 10.0.0.1:8080") != std::string::npos);
    assert(start.find("hostapd -B -P \"$DIR/hostapd.pid\" \"$DIR/hostapd.conf\"") != std::string::npos);
    const std::string stop = stop_script("/d");
    assert(stop.find("-D PREROUTING") != std::string::npos);
    assert(stop.find("managed yes") != std::string::npos);

    // Run-log parsing: the rc marker, or -1 while it is still running.
    assert(parse_rc("line one\nline two\n") == -1);
    assert(parse_rc("boom\n#rc 1\n") == 1);
    assert(parse_rc("#rc 0\n") == 0);
    assert(last_line("setup\nnl80211 driver initialization failed\n#rc 1\n") ==
           "nl80211 driver initialization failed");
    assert(last_line("#rc 0\n").empty());

    // One lease line per connected client; blank lines do not count.
    assert(count_leases("") == 0);
    assert(count_leases("1700000000 aa:bb 10.0.0.10 phone *\n\n1700000001 cc:dd 10.0.0.11 pad *\n") == 2);

    return 0;
}
