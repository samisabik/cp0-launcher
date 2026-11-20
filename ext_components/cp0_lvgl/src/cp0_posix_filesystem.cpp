/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_posix_filesystem.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <new>
#include <sstream>
#include <sys/stat.h>

namespace cp0_posix_filesystem {
namespace {

bool is_dot_entry(const char *name)
{
    return name && name[0] == '.' &&
           (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

std::string join_path(const char *base, const char *name)
{
    std::string path = base ? base : "";
    if (path != "/" && !path.empty() && path.back() != '/') path.push_back('/');
    path += name ? name : "";
    return path;
}

bool directory_entry_is_dir(const char *path, const dirent &entry)
{
    if (entry.d_type == DT_DIR) return true;
    if (entry.d_type != DT_UNKNOWN) return false;
    struct stat status{};
    return stat(join_path(path, entry.d_name).c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

std::string encode_field(const char *value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char character : std::string(value ? value : "")) {
        if (character == '%' || character == '\t' || character == '\n' || character == '\r') {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0f]);
        } else {
            encoded.push_back(static_cast<char>(character));
        }
    }
    return encoded;
}

} // namespace

int encode_directory(const char *path, std::string &output)
{
    DIR *directory = opendir(path);
    if (!directory) return -errno;
    try {
        std::ostringstream encoded;
        while (dirent *entry = readdir(directory)) {
            if (is_dot_entry(entry->d_name)) continue;
            encoded << (directory_entry_is_dir(path, *entry) ? 'D' : 'F') << '\t';
            encoded << encode_field(entry->d_name) << '\n';
        }
        output = encoded.str();
    } catch (const std::bad_alloc &) {
        closedir(directory);
        return -ENOMEM;
    }
    closedir(directory);
    return 0;
}

int read_file_limited(const std::string &path, size_t max_bytes, std::string &output)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return errno ? -errno : -EIO;
    const std::streamoff length = file.tellg();
    if (length < 0) return -EIO;
    if (static_cast<uintmax_t>(length) > max_bytes) return -EFBIG;
    try {
        output.resize(static_cast<size_t>(length));
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
    file.seekg(0);
    if (length > 0 && !file.read(&output[0], length)) {
        output.clear();
        return -EIO;
    }
    return 0;
}

} // namespace cp0_posix_filesystem
