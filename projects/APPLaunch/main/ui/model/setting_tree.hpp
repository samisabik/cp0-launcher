/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

// Ordered tree for the settings menu. An iterator names one node: `begin()`
// and `end()` on it span that node's children, and `++` moves to the next
// sibling. Nodes keep their address until their parent's children are erased.
template <typename T> class SettingTree
{
    struct Node
    {
        explicit Node(T entry) : value(std::move(entry))
        {
        }

        T value;
        Node *next = nullptr;
        std::vector<std::unique_ptr<Node>> children;
    };

public:
    class iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = T *;
        using reference         = T &;

        iterator() = default;

        reference operator*() const
        {
            return node_->value;
        }

        pointer operator->() const
        {
            return &node_->value;
        }

        iterator &operator++()
        {
            node_ = node_->next;
            return *this;
        }

        iterator operator++(int)
        {
            iterator previous = *this;
            node_             = node_->next;
            return previous;
        }

        bool operator==(const iterator &other) const
        {
            return node_ == other.node_;
        }

        bool operator!=(const iterator &other) const
        {
            return node_ != other.node_;
        }

        iterator begin() const
        {
            return iterator(node_ && !node_->children.empty() ? node_->children.front().get() : nullptr);
        }

        iterator end() const
        {
            return iterator();
        }

    private:
        friend class SettingTree;

        explicit iterator(Node *node) : node_(node)
        {
        }

        Node *node_ = nullptr;
    };

    iterator begin() const
    {
        return iterator(head_.get());
    }

    iterator set_head(T entry)
    {
        head_ = std::make_unique<Node>(std::move(entry));
        return iterator(head_.get());
    }

    iterator append_child(const iterator &parent, T entry)
    {
        auto &children = parent.node_->children;
        children.push_back(std::make_unique<Node>(std::move(entry)));
        Node *added = children.back().get();
        if (children.size() > 1) children[children.size() - 2]->next = added;
        return iterator(added);
    }

    void erase_children(const iterator &parent)
    {
        parent.node_->children.clear();
    }

private:
    std::unique_ptr<Node> head_;
};
