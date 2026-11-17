/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/files_model.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static void touch(const std::string &path, const std::string &text = "x")
{
    std::ofstream(path) << text;
}

int main()
{
    assert(files::join("/home/a", "b") == "/home/a/b");
    assert(files::join("/", "b") == "/b");
    assert(files::parent("/home/a/b") == "/home/a");
    assert(files::parent("/home/a/") == "/home");
    assert(files::parent("/home") == "/");
    assert(files::parent("/") == "/");
    assert(files::base_name("/home/a/b/") == "b");

    assert(files::display_path("/home/admin", "/home/admin", 40) == "~");
    assert(files::display_path("/home/admin/Downloads", "/home/admin", 40) == "~/Downloads");
    assert(files::display_path("/home/adminx", "/home/admin", 40) == "/home/adminx");
    assert(files::display_path("/var/lib/very/long/path", "/home/admin", 10) == "~long/path");

    assert(files::format_size(0) == "0B");
    assert(files::format_size(1023) == "1023B");
    assert(files::format_size(2150) == "2.1K");
    assert(files::format_size(14 * 1024) == "14K");
    assert(files::format_size(1887437) == "1.8M");
    assert(files::clip_name("abcdef", 4) == "abc~");
    assert(files::clip_name("abc", 4) == "abc");

    assert(files::valid_name("notes.txt"));
    assert(!files::valid_name(""));
    assert(!files::valid_name("."));
    assert(!files::valid_name(".."));
    assert(!files::valid_name("a/b"));
    assert(!files::valid_name(std::string(256, 'a')));

    std::vector<files::Entry> sorted = {{"b.txt", false}, {"Zeta", true}, {"a.txt", false}, {"alpha", true}};
    files::sort_entries(sorted);
    assert(sorted[0].name == "alpha" && sorted[1].name == "Zeta" && sorted[2].name == "a.txt");

    char tmpl[] = "/tmp/cp0-files-test-XXXXXX";
    const std::string root = mkdtemp(tmpl);
    touch(root + "/notes.txt", "hello\tworld\r\n");
    touch(root + "/.hidden");
    std::string error;
    assert(files::make_dir(root, "docs").empty());
    assert(files::make_dir(root, "docs") == "name taken");
    assert(files::make_dir(root, "a/b") == "invalid name");

    auto listed = files::list_dir(root, false, error);
    assert(error.empty() && listed.size() == 2);
    assert(listed[0].name == "docs" && listed[0].dir);
    assert(listed[1].name == "notes.txt" && listed[1].size == 13);
    assert(files::list_dir(root, true, error).size() == 3);
    files::list_dir(root + "/missing", false, error);
    assert(error == "not found");

    std::string text;
    assert(files::read_preview(root + "/notes.txt", 100, text) && text == "hello world\n");
    {
        std::ofstream bin(root + "/bin.dat", std::ios::binary);
        bin.write("a\0b", 3);
    }
    assert(!files::read_preview(root + "/bin.dat", 100, text));

    assert(files::rename_entry(root, "notes.txt", "todo.txt").empty());
    assert(files::rename_entry(root, "todo.txt", "docs") == "name taken");

    std::string placed;
    assert(files::paste(root + "/todo.txt", root, false, placed).empty() && placed == "todo copy.txt");
    assert(files::paste(root + "/todo.txt", root, false, placed).empty() && placed == "todo copy 2.txt");
    assert(files::paste(root + "/todo.txt", root + "/docs", true, placed).empty() && placed == "todo.txt");
    assert(fs::exists(root + "/docs/todo.txt") && !fs::exists(root + "/todo.txt"));
    assert(files::paste(root + "/docs", root + "/docs", false, placed) == "cannot paste a folder into itself");
    assert(files::paste(root + "/docs", root, false, placed).empty() && placed == "docs copy");
    assert(fs::exists(root + "/docs copy/todo.txt"));

    assert(files::remove_entry(root + "/docs copy").empty());
    assert(!fs::exists(root + "/docs copy"));
    assert(files::remove_entry("/") == "refusing to delete /");

    fs::remove_all(root);
    std::puts("files model: ok");
    return 0;
}
