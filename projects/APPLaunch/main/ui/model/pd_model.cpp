/*
 * SPDX-License-Identifier: MIT
 */

#include "pd_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace pd {

namespace {

std::string lower(const std::string &text)
{
    std::string out = text;
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool is_patch(const std::string &file_name)
{
    const std::size_t suffix = std::strlen(kPatchSuffix);
    if (file_name.size() <= suffix || file_name[0] == '.') return false;
    return file_name.compare(file_name.size() - suffix, suffix, kPatchSuffix) == 0;
}

std::string patch_label(const std::string &file_name)
{
    return is_patch(file_name) ? file_name.substr(0, file_name.size() - std::strlen(kPatchSuffix)) : file_name;
}

std::vector<std::string> patches_in(std::vector<std::string> file_names)
{
    file_names.erase(std::remove_if(file_names.begin(), file_names.end(),
                                    [](const std::string &name) { return !is_patch(name); }),
                     file_names.end());
    std::sort(file_names.begin(), file_names.end(), [](const std::string &a, const std::string &b) {
        const std::string la = lower(a);
        const std::string lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    return file_names;
}

std::string shell_quote(const std::string &text)
{
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

std::string launch_command(const std::string &patch_path, const std::string &output_path)
{
    // Pd reaches PipeWire as a JACK client: its own ALSA backend only lists
    // the hardware card, which PipeWire already holds.
    return "exec pw-jack pd -nogui -noprefs -nrt -jack -inchannels 1 -outchannels 2 -send 'pd dsp 1' -open " +
           shell_quote(patch_path) + " > " + shell_quote(output_path) + " 2>&1";
}

std::string format_elapsed(uint32_t seconds)
{
    char text[24];
    if (seconds >= 3600)
        std::snprintf(text, sizeof(text), "%u:%02u:%02u", static_cast<unsigned>(seconds / 3600),
                      static_cast<unsigned>(seconds / 60 % 60), static_cast<unsigned>(seconds % 60));
    else
        std::snprintf(text, sizeof(text), "%u:%02u", static_cast<unsigned>(seconds / 60),
                      static_cast<unsigned>(seconds % 60));
    return text;
}

Console::Console(std::size_t columns, std::size_t max_lines)
    : columns_(std::max<std::size_t>(columns, 1)), max_lines_(std::max<std::size_t>(max_lines, 1))
{
}

void Console::feed(const char *data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '\n') {
            add_line(pending_);
            pending_.clear();
        } else if (c == '\t') {
            pending_ += ' ';
        } else if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
            pending_ += c;
        }
    }
}

void Console::flush()
{
    if (pending_.empty()) return;
    add_line(pending_);
    pending_.clear();
}

void Console::clear()
{
    pending_.clear();
    lines_.clear();
}

void Console::add_line(const std::string &line)
{
    std::size_t start = 0;
    do {
        lines_.push_back(line.substr(start, columns_));
        start += columns_;
    } while (start < line.size());
    while (lines_.size() > max_lines_) lines_.pop_front();
}

std::vector<std::string> Console::tail(std::size_t count) const
{
    const std::size_t first = lines_.size() > count ? lines_.size() - count : 0;
    return std::vector<std::string>(lines_.begin() + static_cast<std::ptrdiff_t>(first), lines_.end());
}

} // namespace pd
