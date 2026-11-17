// SPDX-License-Identifier: MIT

#include "../main/ui/model/lockscreen_status_model.hpp"

#include <cassert>

using namespace lockscreen_status;

int main()
{
    assert(battery_line(90, 1800, -432) == "90%  4h 10m left");
    assert(battery_line(12, 200, -400) == "12%  30m left");
    assert(battery_line(55, 1000, 600) == "55%  charging");
    assert(battery_line(80, 1500, -5) == "80%");
    assert(battery_line(130, 0, 0) == "100%");
    assert(battery_line(-4, 100, -300) == "0%  20m left");

    assert(uptime_line(59) == "0m");
    assert(uptime_line(12 * 60) == "12m");
    assert(uptime_line(100 * 60) == "1h 40m");
    assert(uptime_line((3 * 24 + 4) * 3600 + 59) == "3d 4h");
    assert(uptime_line(-5) == "0m");
    return 0;
}
