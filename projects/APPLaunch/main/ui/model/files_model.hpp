/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// Filesystem logic for the Files page, free of LVGL so it can be unit-tested
// on the host. Operations return an empty string on success, else a short
// lower-case reason for the footer.
namespace files {

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    std::time_t mtime = 0;
};

// Folders first, then case-insensitive by name. Hidden entries (leading dot)
// are left out unless show_hidden.
std::vector<Entry> list_dir(const std::string &path, bool show_hidden, std::string &error);
void sort_entries(std::vector<Entry> &entries);

std::string join(const std::string &dir, const std::string &name);
std::string parent(const std::string &path);
std::string base_name(const std::string &path);
// The path with $HOME shown as "~", cut from the left to max_chars.
std::string display_path(const std::string &path, const std::string &home, std::size_t max_chars);
std::string format_size(uint64_t bytes);
std::string format_time(std::time_t when);
// Name cut to max_chars with a trailing "~" when it does not fit.
std::string clip_name(const std::string &name, std::size_t max_chars);

// A new file or folder name: not empty, no "/", not "." or "..", at most 255 bytes.
bool valid_name(const std::string &name);

std::string make_dir(const std::string &dir, const std::string &name);
std::string rename_entry(const std::string &dir, const std::string &from, const std::string &to);
std::string remove_entry(const std::string &path);
// Copies or moves `source` into `dest_dir`, keeping its name; a clash gets a
// " copy", " copy 2", ... suffix. A folder is never placed inside itself.
std::string paste(const std::string &source, const std::string &dest_dir, bool move, std::string &placed_name);
std::string free_name(const std::string &dir, const std::string &name);

// Up to max_bytes of a file as text, or false when it looks binary.
bool read_preview(const std::string &path, std::size_t max_bytes, std::string &text);

} // namespace files
