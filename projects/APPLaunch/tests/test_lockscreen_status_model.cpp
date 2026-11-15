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

    assert(recordings_line(0, 0) == "none");
    assert(recordings_line(1, 0) == "1 clip");
    assert(recordings_line(3, 1) == "3 clips, 1 to transcribe");
    return 0;
}
