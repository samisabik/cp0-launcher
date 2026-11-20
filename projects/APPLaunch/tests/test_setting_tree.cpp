/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/setting_tree.hpp"

#include <cassert>
#include <iterator>
#include <string>
#include <vector>

namespace {

using Tree = SettingTree<std::string>;

std::vector<std::string> children_of(const Tree::iterator &node)
{
    std::vector<std::string> labels;
    for (auto child = node.begin(); child != node.end(); ++child) labels.push_back(*child);
    return labels;
}

} // namespace

int main()
{
    Tree tree;
    assert(tree.begin() == Tree::iterator());

    const Tree::iterator root = tree.set_head("settings");
    assert(tree.begin() == root && *root == "settings");
    assert(root.begin() == root.end());

    const Tree::iterator screen = tree.append_child(root, "screen");
    const Tree::iterator sound  = tree.append_child(root, "sound");
    const Tree::iterator wifi   = tree.append_child(root, "wi-fi");
    tree.append_child(screen, "brightness");
    tree.append_child(screen, "auto-lock");

    // Children come back in insertion order and stop at the last sibling.
    assert((children_of(root) == std::vector<std::string>{"screen", "sound", "wi-fi"}));
    assert((children_of(screen) == std::vector<std::string>{"brightness", "auto-lock"}));
    assert(children_of(sound).empty());

    // Index access the way the menu selects a row.
    assert(std::next(root.begin(), 1) == sound);
    assert(std::distance(root.begin(), wifi) == 2);
    assert(std::distance(root.begin(), root.end()) == 3);

    // A node keeps its address while siblings and children are added.
    const std::string *sound_address = &*sound;
    for (int i = 0; i < 64; ++i) tree.append_child(root, "extra");
    tree.append_child(sound, "volume");
    assert(&*sound == sound_address && *sound == "sound");
    sound->append(" out");
    assert(*std::next(root.begin(), 1) == "sound out");

    // Erasing children leaves the node and its siblings in place.
    tree.erase_children(screen);
    assert(screen.begin() == screen.end());
    assert(*screen == "screen" && std::next(root.begin(), 1) == sound);
    tree.append_child(screen, "accent");
    assert((children_of(screen) == std::vector<std::string>{"accent"}));

    return 0;
}
