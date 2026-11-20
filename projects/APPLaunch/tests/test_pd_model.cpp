/*
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/pd_model.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

void patch_names()
{
    assert(pd::is_patch("sine.pd"));
    assert(!pd::is_patch(".pd"));
    assert(!pd::is_patch(".hidden.pd"));
    assert(!pd::is_patch("sine.pd~"));
    assert(!pd::is_patch("notes.txt"));
    assert(pd::patch_label("sine.pd") == "sine");
    assert(pd::patch_label("a.b.pd") == "a.b");
    assert(pd::patch_label("notes.txt") == "notes.txt");

    const std::vector<std::string> sorted = pd::patches_in({"zap.pd", "Bass.pd", "readme.md", "arp.pd", ".x.pd"});
    assert((sorted == std::vector<std::string>{"arp.pd", "Bass.pd", "zap.pd"}));
}

void command()
{
    assert(pd::shell_quote("plain") == "'plain'");
    assert(pd::shell_quote("it's") == "'it'\\''s'");

    const std::string cmd = pd::launch_command("/home/u/pd/my patch.pd", "/run/user/1000/out");
    assert(cmd.rfind("exec pw-jack pd -nogui ", 0) == 0);
    assert(cmd.find("-send 'pd dsp 1'") != std::string::npos);
    assert(cmd.find("-open '/home/u/pd/my patch.pd' > '/run/user/1000/out' 2>&1") != std::string::npos);
}

void elapsed()
{
    assert(pd::format_elapsed(0) == "0:00");
    assert(pd::format_elapsed(75) == "1:15");
    assert(pd::format_elapsed(3599) == "59:59");
    assert(pd::format_elapsed(3725) == "1:02:05");
}

void console()
{
    pd::Console console(10, 4);
    console.feed("hel", 3);
    assert(console.size() == 0);
    console.feed("lo\nwor", 6);
    assert((console.tail(5) == std::vector<std::string>{"hello"}));
    console.feed("ld\r\n", 4);
    assert((console.tail(5) == std::vector<std::string>{"hello", "world"}));

    console.feed("0123456789abcde\n", 16);
    assert((console.tail(2) == std::vector<std::string>{"0123456789", "abcde"}));

    console.feed("a\tb\n\n", 5);
    assert((console.tail(4) == std::vector<std::string>{"0123456789", "abcde", "a b", ""}));
    assert(console.size() == 4);

    console.feed("tail", 4);
    console.flush();
    console.flush();
    assert(console.tail(1).front() == "tail");
    assert(console.size() == 4);

    console.clear();
    assert(console.size() == 0);
    assert(console.tail(3).empty());
}

} // namespace

int main()
{
    patch_names();
    command();
    elapsed();
    console();
    return 0;
}
