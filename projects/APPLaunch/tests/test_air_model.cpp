// SPDX-License-Identifier: MIT

#include "../main/ui/model/air_model.hpp"

#include <cassert>
#include <cmath>
#include <string>
#include <vector>

using namespace air;

namespace {

bool near(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

Network net(const char *ssid, int channel, int signal, const char *security = "wpa2")
{
    Network network;
    network.ssid = ssid;
    network.channel = channel;
    network.signal = signal;
    network.security = security;
    return network;
}

void test_split_nmcli_line()
{
    std::vector<std::string> f = split_nmcli_line("cp0-lab:6:92:WPA2 WPA3");
    assert(f.size() == 4 && f[0] == "cp0-lab" && f[1] == "6" && f[2] == "92" && f[3] == "WPA2 WPA3");

    // Escaped colons and backslashes belong to the value.
    f = split_nmcli_line(R"(caf\: 5G\\net:11:40:WPA2)");
    assert(f.size() == 4 && f[0] == R"(caf: 5G\net)" && f[1] == "11");

    f = split_nmcli_line(":1:55:");
    assert(f.size() == 4 && f[0].empty() && f[3].empty());

    f = split_nmcli_line("");
    assert(f.size() == 1 && f[0].empty());
    // A trailing lone backslash is kept as it is.
    f = split_nmcli_line("a\\");
    assert(f.size() == 1 && f[0] == "a\\");
}

void test_parse_nmcli()
{
    const std::string output = "cp0-lab:6:92:WPA2 WPA3\n"
                               "attic:1:64:WPA2\n"
                               ":11:48:WPA1 WPA2\n"
                               "my\\:net:36:71:WPA3\r\n"
                               "guest:6:38:\n"
                               "legacy:3:30:WEP\n"
                               "open-dash:9:22:--\n"
                               "\n"
                               "broken line without fields\n"
                               "bad-channel:x:50:WPA2\n"
                               "bad-signal:6:strong:WPA2\n"
                               "zero-channel:0:50:WPA2\n"
                               "too:many:fields:1:2\n"
                               "loud:13:250:WPA2\n";
    const std::vector<Network> nets = parse_nmcli(output);
    assert(nets.size() == 8);
    assert(nets[0].ssid == "cp0-lab" && nets[0].channel == 6 && nets[0].signal == 92 && nets[0].security == "wpa3");
    assert(nets[1].ssid == "attic" && nets[1].security == "wpa2");
    assert(nets[2].ssid.empty() && nets[2].channel == 11 && nets[2].security == "wpa2");
    assert(nets[3].ssid == "my:net" && nets[3].channel == 36 && nets[3].security == "wpa3");
    assert(nets[4].ssid == "guest" && nets[4].security == "open");
    assert(nets[5].security == "wep");
    assert(nets[6].security == "open");
    assert(nets[7].ssid == "loud" && nets[7].signal == 100);

    assert(parse_nmcli("").empty());
    assert(parse_nmcli("Error: NetworkManager is not running.\n").empty());
    assert(scan_cmd() == "nmcli -t -f SSID,CHAN,SIGNAL,SECURITY device wifi list --rescan auto");
}

void test_security_label()
{
    assert(security_label("WPA2 WPA3") == "wpa3");
    assert(security_label("WPA3") == "wpa3");
    assert(security_label("WPA1 WPA2") == "wpa2");
    assert(security_label("WPA1") == "wpa");
    assert(security_label("WEP") == "wep");
    assert(security_label("WPA2 802.1X") == "wpa2");
    assert(security_label("802.1X") == "802.1x");
    assert(security_label("") == "open");
    assert(security_label("--") == "open");
}

void test_bands()
{
    assert(band_of(1) == Band::G24 && band_of(13) == Band::G24 && band_of(14) == Band::G24);
    assert(band_of(36) == Band::G5 && band_of(165) == Band::G5 && band_of(177) == Band::G5);
    assert(band_of(0) == Band::Unknown && band_of(-3) == Band::Unknown);
    assert(band_of(15) == Band::Unknown && band_of(31) == Band::Unknown && band_of(178) == Band::Unknown);

    assert(centre_mhz(1) == 2412);
    assert(centre_mhz(6) == 2437);
    assert(centre_mhz(11) == 2462);
    assert(centre_mhz(13) == 2472);
    assert(centre_mhz(14) == 2484);
    assert(centre_mhz(36) == 5180);
    assert(centre_mhz(100) == 5500);
    assert(centre_mhz(165) == 5825);
    assert(centre_mhz(0) == 0 && centre_mhz(20) == 0);

    assert(std::string(band_label(Band::G24)) == "2.4 ghz");
    assert(std::string(band_label(Band::G5)) == "5 ghz");

    const std::vector<Network> all = {net("a", 1, 50), net("b", 36, 60), net("c", 11, 70), net("d", 149, 20)};
    const std::vector<Network> low = in_band(all, Band::G24);
    const std::vector<Network> high = in_band(all, Band::G5);
    assert(low.size() == 2 && low[0].ssid == "a" && low[1].ssid == "c");
    assert(high.size() == 2 && high[0].ssid == "b" && high[1].ssid == "d");
}

void test_signal_conversion()
{
    assert(percent_to_dbm(100) == -50);
    assert(percent_to_dbm(84) == -58);
    assert(percent_to_dbm(0) == -100);
    assert(percent_to_dbm(-20) == -100);
    assert(percent_to_dbm(400) == -50);
    assert(dbm_to_percent(-50) == 100);
    assert(dbm_to_percent(-58) == 84);
    assert(dbm_to_percent(-100) == 0);
    assert(dbm_to_percent(-120) == 0);
    assert(dbm_to_percent(-20) == 100);
    for (int percent = 0; percent <= 100; percent += 2) assert(dbm_to_percent(percent_to_dbm(percent)) == percent);
}

void test_sorting()
{
    std::vector<Network> nets = {net("guest", 6, 38), net("Attic", 1, 64), net("", 11, 48),
                                 net("cp0-lab", 6, 92), net("beta", 11, 64)};
    sort_networks(nets, Sort::Signal);
    assert(nets[0].ssid == "cp0-lab");
    // Equal signal falls back to the name.
    assert(nets[1].ssid == "Attic" && nets[2].ssid == "beta");
    assert(nets[3].ssid.empty() && nets[4].ssid == "guest");

    sort_networks(nets, Sort::Channel);
    assert(nets[0].channel == 1);
    assert(nets[1].channel == 6 && nets[1].ssid == "cp0-lab");
    assert(nets[2].channel == 6 && nets[2].ssid == "guest");
    assert(nets[3].channel == 11 && nets[3].ssid == "beta");
    assert(nets[4].channel == 11 && nets[4].ssid.empty());

    sort_networks(nets, Sort::Name);
    assert(nets[0].ssid == "Attic" && nets[1].ssid == "beta" && nets[2].ssid == "cp0-lab" && nets[3].ssid == "guest");
    assert(nets[4].ssid.empty());

    assert(next_sort(Sort::Signal) == Sort::Channel);
    assert(next_sort(Sort::Channel) == Sort::Name);
    assert(next_sort(Sort::Name) == Sort::Signal);
    assert(std::string(sort_label(Sort::Signal)) == "signal");
    assert(std::string(sort_label(Sort::Channel)) == "channel");
    assert(std::string(sort_label(Sort::Name)) == "name");
}

void test_congestion()
{
    assert(near(overlap(0.0f), 1.0f));
    assert(near(overlap(1.0f), 17.0f / 22.0f));
    assert(near(overlap(-1.0f), 17.0f / 22.0f));
    assert(near(overlap(4.0f), 2.0f / 22.0f));
    assert(overlap(5.0f) == 0.0f);
    assert(overlap(-9.0f) == 0.0f);

    auto scores = congestion({});
    for (const float score : scores) assert(score == 0.0f);
    assert(quietest_channel(scores) == 1);

    // One full-strength network on channel 6.
    scores = congestion({net("a", 6, 100)});
    assert(scores[0] == 0.0f);
    assert(near(scores[6], 1.0f));
    assert(near(scores[5], 17.0f / 22.0f) && near(scores[7], 17.0f / 22.0f));
    assert(near(scores[2], 2.0f / 22.0f) && near(scores[10], 2.0f / 22.0f));
    assert(scores[1] == 0.0f && scores[11] == 0.0f && scores[13] == 0.0f);
    // Channels 1 and 11 tie at zero; the lower one is reported.
    assert(quietest_channel(scores) == 1);

    // Busy 1 and 6, weak 11; a 5 GHz network does not count.
    scores = congestion({net("a", 1, 80), net("b", 6, 90), net("c", 6, 40), net("d", 11, 20), net("e", 36, 100)});
    assert(near(scores[1], 0.8f));
    assert(near(scores[6], 1.3f));
    assert(near(scores[11], 0.2f));
    assert(near(scores[3], 0.8f * 12.0f / 22.0f + 1.3f * 7.0f / 22.0f));
    assert(quietest_channel(scores) == 11);

    scores = congestion({net("a", 1, 10), net("b", 11, 90), net("c", 13, 90)});
    assert(quietest_channel(scores) == 6);
    // A channel 14 network still weighs on 11..13.
    scores = congestion({net("j", 14, 100)});
    assert(near(scores[13], 17.0f / 22.0f) && near(scores[11], 7.0f / 22.0f));
}

void test_curve()
{
    assert(near(curve_height(6, 6.0f, 40.0f), 40.0f));
    assert(near(curve_height(6, 7.0f, 40.0f), 30.0f));
    assert(near(curve_height(6, 5.0f, 40.0f), 30.0f));
    assert(near(curve_height(6, 6.5f, 40.0f), 40.0f * (1.0f - 0.0625f)));
    assert(curve_height(6, 8.0f, 40.0f) == 0.0f);
    assert(curve_height(6, 4.0f, 40.0f) == 0.0f);
    assert(curve_height(6, 12.0f, 40.0f) == 0.0f);
    assert(curve_height(6, 6.0f, 0.0f) == 0.0f);
    assert(curve_height(6, 6.0f, -5.0f) == 0.0f);
    // Symmetric and never above the peak.
    for (float d = 0.0f; d <= 2.0f; d += 0.1f) {
        const float left = curve_height(3, 3.0f - d, 25.0f);
        const float right = curve_height(3, 3.0f + d, 25.0f);
        assert(near(left, right, 1e-3f));
        assert(left >= 0.0f && left <= 25.0f);
    }

    assert(near(peak_height(100, 58.0f), 58.0f));
    assert(near(peak_height(50, 58.0f), 29.0f));
    assert(near(peak_height(0, 58.0f), 2.0f));
    assert(near(peak_height(300, 58.0f), 58.0f));
    assert(peak_height(50, 0.0f) == 0.0f);
}

void test_channel_bars()
{
    const std::vector<Network> nets = {net("a", 149, 40), net("b", 36, 70), net("c", 36, 55),
                                       net("d", 100, 20), net("e", 44, 61), net("f", 36, 90)};
    std::vector<ChannelBar> bars = channel_bars(nets, 13);
    assert(bars.size() == 4);
    assert(bars[0].channel == 36 && bars[0].count == 3 && bars[0].signal == 90);
    assert(bars[1].channel == 44 && bars[1].count == 1 && bars[1].signal == 61);
    assert(bars[2].channel == 100 && bars[2].signal == 20);
    assert(bars[3].channel == 149 && bars[3].signal == 40);

    // With room for two, the two strongest channels stay, in channel order.
    bars = channel_bars(nets, 2);
    assert(bars.size() == 2 && bars[0].channel == 36 && bars[1].channel == 44);
    assert(channel_bars({}, 13).empty());
    assert(channel_bars(nets, 0).empty());
}

void test_formatting()
{
    assert(fit("abc", 5) == "abc  ");
    assert(fit("abcdefgh", 5) == "abcde");
    assert(fit("abc", 5, false) == "abc");
    assert(fit("", 3) == "   ");
    // Two-byte characters count as one column and are never cut in half.
    assert(fit("caf\xC3\xA9s", 4) == "caf\xC3\xA9");
    assert(fit("\xC3\xA9\xC3\xA9", 3) == "\xC3\xA9\xC3\xA9 ");
    assert(fit("\xE2\x82\xAC" "1", 1) == "\xE2\x82\xAC");
    // Broken UTF-8 and control characters turn into '?'.
    assert(fit("a\xC3", 4) == "a?  ");
    assert(fit("a\x80\tb", 4) == "a??b");
    assert(fit("\xF0\x9F\x98", 4, false) == "???");

    assert(display_name(net("", 1, 10)) == "(hidden)");
    assert(display_name(net("x", 1, 10)) == "x");
    //                                     ssid(12)+1   chan(7) sec(7) dbm(4)
    assert(row_text(net("cp0-lab", 6, 92, "wpa3")) == "cp0-lab      ch 6   wpa3    -54");
    assert(row_text(net("a-very-long-network-name", 149, 100, "open")) == "a-very-long- ch 149 open    -50");
    assert(row_text(net("", 11, 0, "wpa2")) == "(hidden)     ch 11  wpa2   -100");
    assert(row_text(net("x", 1, 1)).size() == 31);
}

void test_bluetooth()
{
    const std::string payload = "AA:BB:CC:DD:EE:01\t-71\t0\t0\t0\tKitchen Speaker\n"
                                "AA:BB:CC:DD:EE:02\t-48\t1\t1\t1\tHeadphones\n"
                                "AA:BB:CC:DD:EE:03\t0\t0\t1\t1\tOld Keyboard\n"
                                "AA:BB:CC:DD:EE:04\t-63\t0\t0\t0\tAA-BB-CC-DD-EE-04\n"
                                "AA:BB:CC:DD:EE:05\t-90\t0\t0\t0\t\n"
                                "AA:BB:CC:DD:EE:06\tloud\t0\t0\t0\n"
                                "\n"
                                "short\tline\n"
                                "\t-40\t0\t0\t0\tno address\n";
    std::vector<BleDevice> devices = parse_bt_devices(payload);
    assert(devices.size() == 6);
    assert(devices[0].address == "AA:BB:CC:DD:EE:01" && devices[0].rssi == -71 && devices[0].name == "Kitchen Speaker");
    assert(!devices[0].connected && !devices[0].paired);
    assert(devices[1].connected && devices[1].paired && devices[1].rssi == -48);
    assert(devices[2].rssi == 0 && devices[2].paired);
    assert(devices[4].name.empty());
    // An unreadable RSSI and a missing name field leave the defaults.
    assert(devices[5].rssi == 0 && devices[5].name.empty());

    assert(ble_label(devices[0]) == "Kitchen Speaker");
    assert(ble_label(devices[3]) == "aa:bb:cc:dd:ee:04");
    assert(ble_label(devices[4]) == "aa:bb:cc:dd:ee:05");

    sort_ble(devices);
    assert(devices[0].name == "Headphones");
    assert(devices[1].address == "AA:BB:CC:DD:EE:04");
    assert(devices[2].name == "Kitchen Speaker");
    assert(devices[3].address == "AA:BB:CC:DD:EE:05");
    // No reading: last, by label.
    assert(devices[4].address == "AA:BB:CC:DD:EE:06");
    assert(devices[5].name == "Old Keyboard");

    assert(parse_bt_devices("").empty());
    assert(parse_bt_devices("bluetooth backend failure").empty());

    bool powered = false;
    assert(parse_bt_status("1\tAA:BB:CC:DD:EE:FF\t0\tcp0", powered) && powered);
    assert(parse_bt_status("0\tAA:BB:CC:DD:EE:FF\t0\tcp0\n", powered) && !powered);
    assert(parse_bt_status("1\t\t0\t", powered) && powered);
    assert(!parse_bt_status("", powered));
    assert(!parse_bt_status("invalid bt session", powered));
    assert(!parse_bt_status("on\taddr", powered));

    assert(rssi_fraction(0) == 0.0f);
    assert(rssi_fraction(-100) == 0.0f);
    assert(rssi_fraction(-120) == 0.0f);
    assert(near(rssi_fraction(-70), 0.5f));
    assert(rssi_fraction(-40) == 1.0f);
    assert(rssi_fraction(-20) == 1.0f);

    //                                  label(22)+1             state(7) rssi(4)
    assert(ble_row_text(devices[0]) == "Headphones             conn    -48");
    assert(ble_row_text(devices[5]) == "Old Keyboard           paired   --");
    assert(ble_row_text(devices[3]) == "aa:bb:cc:dd:ee:05              -90");
    assert(ble_row_text(devices[3]).size() == 34);
}

} // namespace

int main()
{
    test_split_nmcli_line();
    test_parse_nmcli();
    test_security_label();
    test_bands();
    test_signal_conversion();
    test_sorting();
    test_congestion();
    test_curve();
    test_channel_bars();
    test_formatting();
    test_bluetooth();
    return 0;
}
