// SPDX-License-Identifier: MIT

#include "../main/ui/model/sdr_model.hpp"

#include <cassert>
#include <string>

using namespace sdr;

int main()
{
    assert(clamp_hz(0) == kMinHz);
    assert(clamp_hz(2000000000) == kMaxHz);
    assert(clamp_hz(433920000) == 433920000u);

    assert(format_mhz(88500000) == "88.500");
    assert(format_hz(433920000) == "433.920 MHz");
    assert(format_hz(500000) == "500 kHz");
    assert(gain_label(-1) == "auto");
    assert(gain_label(30) == "30 dB");

    assert(demod_label(Demod::WFM) == std::string("wfm"));
    assert(demod_label(Demod::AM) == std::string("am"));

    const std::string power = power_cmd(433920000, 2000000, 30, "/tmp/p.csv");
    assert(power.find("rtl_power -f 432920000:434920000:6250 -i 1 -g 30 '/tmp/p.csv'") != std::string::npos);
    // Auto gain omits -g.
    assert(power_cmd(100000000, 1000000, -1, "/x").find(" -g ") == std::string::npos);

    const std::string listen = listen_cmd(98000000, Demod::WFM, 20);
    assert(listen.find("rtl_fm -M wbfm -f 98000000 -s 200000 -r 48000 -g 20 -") != std::string::npos);
    assert(listen.find("aplay -q -r 48000 -f S16_LE -t raw -c 1") != std::string::npos);
    assert(listen_cmd(124000000, Demod::AM, -1).find("-M am") != std::string::npos);

    assert(decode_cmd(433920000, 40, "/tmp/d").find("rtl_433 -f 433920000 -F json -g 40 > '/tmp/d'") !=
           std::string::npos);

    const Detect none = parse_detect("No supported devices found.\n");
    assert(!none.present);
    const Detect found =
        parse_detect("Found 1 device(s):\n  0:  Realtek, RTL2838UHIDIR, SN: 1\n\nUsing device 0\n");
    assert(found.present && found.name.find("RTL2838") != std::string::npos);

    const PowerRow row = parse_power_row("2024-01-01, 12:00:00, 433000000, 434000000, 2000.00, 20, -40, -38, -52");
    assert(row.valid && row.low_hz == 433000000u && row.step_hz == 2000u);
    assert(row.db.size() == 3 && row.db[0] == -40 && row.db[2] == -52);
    assert(!parse_power_row("bad,line").valid);

    const std::vector<int> cols = power_columns(row, 6);
    assert(static_cast<int>(cols.size()) == 6 && cols.front() == -40 && cols.back() == -52);

    assert(level_index(-120, -110, -40, 64) == 0);
    assert(level_index(-40, -110, -40, 64) == 63);

    assert(summarize_433("{\"model\":\"Nexus-TH\",\"id\":42,\"temperature_C\":21.3,\"humidity\":55}") ==
           "Nexus-TH id 42 21.3C 55%");
    assert(summarize_433("{\"time\":\"x\"}").empty());
    assert(summarize_433("{\"model\":\"Car-Remote\",\"button\":2}") == "Car-Remote btn 2");
    return 0;
}
