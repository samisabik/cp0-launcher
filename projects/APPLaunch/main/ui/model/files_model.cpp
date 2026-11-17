/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "files_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace files {
namespace {

std::string lower(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string reason(const std::error_code &error)
{
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted)
        return "permission denied";
    if (error == std::errc::no_space_on_device) return "disk full";
    if (error == std::errc::file_exists) return "name taken";
    if (error == std::errc::no_such_file_or_directory) return "not found";
    if (error == std::errc::directory_not_empty) return "folder not empty";
    return lower(error.message());
}

bool exists(const std::string &path)
{
    std::error_code error;
    return fs::symlink_status(path, error).type() != fs::file_type::not_found && !error;
}

} // namespace

void sort_entries(std::vector<Entry> &entries)
{
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir) return a.dir;
        const std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
}

std::vector<Entry> list_dir(const std::string &path, bool show_hidden, std::string &error)
{
    std::vector<Entry> entries;
    error.clear();
    std::error_code code;
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, code);
    if (code) {
        error = reason(code);
        return entries;
    }
    for (const fs::directory_iterator end; it != end; it.increment(code)) {
        if (code) break;
        Entry entry;
        entry.name = it->path().filename().string();
        if (!show_hidden && !entry.name.empty() && entry.name[0] == '.') continue;
        std::error_code ignored;
        entry.dir = it->is_directory(ignored);
        if (!entry.dir) {
            const auto size = it->file_size(ignored);
            entry.size = ignored ? 0 : static_cast<uint64_t>(size);
        }
        const auto written = fs::last_write_time(it->path(), ignored);
        if (!ignored) {
            const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            entry.mtime = std::chrono::system_clock::to_time_t(system);
        }
        entries.push_back(std::move(entry));
    }
    sort_entries(entries);
    return entries;
}

std::string join(const std::string &dir, const std::string &name)
{
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string parent(const std::string &path)
{
    if (path.empty() || path == "/") return "/";
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
    const std::size_t slash = trimmed.rfind('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : trimmed.substr(0, slash);
}

std::string base_name(const std::string &path)
{
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
    const std::size_t slash = trimmed.rfind('/');
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

std::string display_path(const std::string &path, const std::string &home, std::size_t max_chars)
{
    std::string shown = path;
    if (!home.empty() && home != "/" &&
        (path == home || (path.size() > home.size() && path.compare(0, home.size(), home) == 0 &&
                          path[home.size()] == '/')))
        shown = "~" + path.substr(home.size());
    if (max_chars == 0) return {};
    if (shown.size() <= max_chars) return shown;
    if (max_chars == 1) return "~";
    return "~" + shown.substr(shown.size() - (max_chars - 1));
}

std::string format_size(uint64_t bytes)
{
    static const char *const kUnits[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char text[16];
    if (unit == 0)
        std::snprintf(text, sizeof(text), "%lluB", static_cast<unsigned long long>(bytes));
    else if (value < 10.0)
        std::snprintf(text, sizeof(text), "%.1f%s", value, kUnits[unit]);
    else
        std::snprintf(text, sizeof(text), "%.0f%s", value, kUnits[unit]);
    return text;
}

std::string format_time(std::time_t when)
{
    if (when <= 0) return "--";
    std::tm local{};
    localtime_r(&when, &local);
    char text[24];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

std::string clip_name(const std::string &name, std::size_t max_chars)
{
    if (name.size() <= max_chars) return name;
    if (max_chars == 0) return {};
    return name.substr(0, max_chars - 1) + "~";
}

bool valid_name(const std::string &name)
{
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
    for (const unsigned char c : name)
        if (c == '/' || c < 0x20 || c == 0x7f) return false;
    return true;
}

std::string make_dir(const std::string &dir, const std::string &name)
{
    if (!valid_name(name)) return "invalid name";
    const std::string target = join(dir, name);
    if (exists(target)) return "name taken";
    std::error_code error;
    fs::create_directory(target, error);
    return error ? reason(error) : std::string();
}

std::string rename_entry(const std::string &dir, const std::string &from, const std::string &to)
{
    if (!valid_name(to)) return "invalid name";
    if (from == to) return {};
    const std::string target = join(dir, to);
    if (exists(target)) return "name taken";
    std::error_code error;
    fs::rename(join(dir, from), target, error);
    return error ? reason(error) : std::string();
}

std::string remove_entry(const std::string &path)
{
    if (path.empty() || path == "/") return "refusing to delete /";
    std::error_code error;
    fs::remove_all(path, error);
    return error ? reason(error) : std::string();
}

std::string free_name(const std::string &dir, const std::string &name)
{
    if (!exists(join(dir, name))) return name;
    // "notes.txt" -> "notes copy.txt"; folders and dotfiles keep the suffix at the end.
    const std::size_t dot = name.rfind('.');
    const bool has_ext = dot != std::string::npos && dot > 0 && !fs::is_directory(join(dir, name));
    const std::string stem = has_ext ? name.substr(0, dot) : name;
    const std::string ext = has_ext ? name.substr(dot) : std::string();
    for (int n = 1; n < 1000; ++n) {
        const std::string candidate = stem + (n == 1 ? " copy" : " copy " + std::to_string(n)) + ext;
        if (!exists(join(dir, candidate))) return candidate;
    }
    return {};
}

std::string paste(const std::string &source, const std::string &dest_dir, bool move, std::string &placed_name)
{
    placed_name.clear();
    if (!exists(source)) return "source is gone";
    std::error_code error;
    const fs::path src = fs::weakly_canonical(source, error);
    const fs::path dst = fs::weakly_canonical(dest_dir, error);
    if (error) return reason(error);
    const std::string src_text = src.string(), dst_text = dst.string();
    if (dst_text == src_text ||
        (dst_text.size() > src_text.size() && dst_text.compare(0, src_text.size(), src_text) == 0 &&
         dst_text[src_text.size()] == '/'))
        return "cannot paste a folder into itself";
    if (move && parent(src_text) == dst_text) {
        placed_name = base_name(src_text);
        return {};
    }
    const std::string name = free_name(dest_dir, base_name(src_text));
    if (name.empty()) return "no free name";
    const std::string target = join(dest_dir, name);
    if (move) {
        fs::rename(src, target, error);
        if (!error) {
            placed_name = name;
            return {};
        }
        if (error != std::errc::cross_device_link) return reason(error);
        error.clear();
    }
    fs::copy(src, target, fs::copy_options::recursive | fs::copy_options::copy_symlinks, error);
    if (error) {
        std::error_code ignored;
        fs::remove_all(target, ignored);
        return reason(error);
    }
    if (move) {
        fs::remove_all(src, error);
        if (error) return "copied, but could not remove source";
    }
    placed_name = name;
    return {};
}

bool read_preview(const std::string &path, std::size_t max_bytes, std::string &text)
{
    text.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    text.resize(max_bytes);
    input.read(&text[0], static_cast<std::streamsize>(max_bytes));
    text.resize(static_cast<std::size_t>(input.gcount()));
    for (const unsigned char c : text)
        if (c == 0) return false;
    std::string clean;
    clean.reserve(text.size());
    for (const char c : text)
        if (c != '\r') clean.push_back(c == '\t' ? ' ' : c);
    text.swap(clean);
    return true;
}

} // namespace files
