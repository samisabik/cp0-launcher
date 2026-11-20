/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Logic of the Pd page: which files are patches, the command that runs one,
// and the console that collects what the patch prints.
namespace pd {

constexpr const char *kPatchSuffix = ".pd";

// True for a visible file name ending in ".pd".
bool is_patch(const std::string &file_name);

// File name without the ".pd" suffix.
std::string patch_label(const std::string &file_name);

// Patch file names in a directory listing, sorted without regard to case.
std::vector<std::string> patches_in(std::vector<std::string> file_names);

// Wraps text in single quotes for /bin/sh.
std::string shell_quote(const std::string &text);

// Shell command that runs a patch without the Pd GUI, with DSP on, and sends
// everything Pd prints to output_path.
std::string launch_command(const std::string &patch_path, const std::string &output_path);

// "m:ss", or "h:mm:ss" from one hour.
std::string format_elapsed(uint32_t seconds);

// Line buffer for the output of a running patch. Bytes arrive in arbitrary
// chunks; whole lines are wrapped to the panel width and the oldest dropped.
class Console
{
public:
    Console(std::size_t columns, std::size_t max_lines);

    void feed(const char *data, std::size_t size);
    // Ends the line in progress, as at the end of a run.
    void flush();
    void clear();
    void add_line(const std::string &line);

    // The newest `count` lines, oldest first.
    std::vector<std::string> tail(std::size_t count) const;
    std::size_t size() const
    {
        return lines_.size();
    }

private:
    std::size_t columns_;
    std::size_t max_lines_;
    std::string pending_;
    std::deque<std::string> lines_;
};

} // namespace pd
