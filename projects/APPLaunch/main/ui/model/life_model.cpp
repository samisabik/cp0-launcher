/*
 * SPDX-License-Identifier: MIT
 */

#include "life_model.hpp"

LifeGrid::LifeGrid(int width, int height)
    : width_(width > 0 ? width : 1), height_(height > 0 ? height : 1),
      cells_(static_cast<std::size_t>(width_) * height_, kMaxDeadAge), next_(cells_.size(), kMaxDeadAge)
{
}

std::size_t LifeGrid::index(int x, int y) const
{
    x %= width_;
    y %= height_;
    if (x < 0) x += width_;
    if (y < 0) y += height_;
    return static_cast<std::size_t>(y) * width_ + x;
}

void LifeGrid::clear()
{
    for (int8_t &cell : cells_) cell = kMaxDeadAge;
    population_ = 0;
    generation_ = 0;
    stale_steps_ = 0;
    hashes_[0] = hashes_[1] = 0;
}

void LifeGrid::set(int x, int y, bool on)
{
    int8_t &cell = cells_[index(x, y)];
    if (on == (cell > 0)) return;
    cell = on ? 1 : 0;
    population_ += on ? 1 : static_cast<std::size_t>(-1);
}

void LifeGrid::randomize(uint32_t seed, int density_percent)
{
    clear();
    uint32_t state = seed ? seed : 0x9E3779B9u;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            if (static_cast<int>(state % 100) < density_percent) set(x, y, true);
        }
    }
}

void LifeGrid::glider(int x, int y)
{
    set(x + 1, y, true);
    set(x + 2, y + 1, true);
    set(x, y + 2, true);
    set(x + 1, y + 2, true);
    set(x + 2, y + 2, true);
}

uint64_t LifeGrid::live_hash() const
{
    uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (cells_[i] <= 0) continue;
        hash ^= i;
        hash *= 1099511628211ull;
    }
    return hash;
}

void LifeGrid::step()
{
    population_ = 0;
    for (int y = 0; y < height_; ++y) {
        const int up = (y + height_ - 1) % height_;
        const int down = (y + 1) % height_;
        for (int x = 0; x < width_; ++x) {
            const int left = (x + width_ - 1) % width_;
            const int right = (x + 1) % width_;
            const int neighbours =
                (cells_[static_cast<std::size_t>(up) * width_ + left] > 0) +
                (cells_[static_cast<std::size_t>(up) * width_ + x] > 0) +
                (cells_[static_cast<std::size_t>(up) * width_ + right] > 0) +
                (cells_[static_cast<std::size_t>(y) * width_ + left] > 0) +
                (cells_[static_cast<std::size_t>(y) * width_ + right] > 0) +
                (cells_[static_cast<std::size_t>(down) * width_ + left] > 0) +
                (cells_[static_cast<std::size_t>(down) * width_ + x] > 0) +
                (cells_[static_cast<std::size_t>(down) * width_ + right] > 0);
            const std::size_t i = static_cast<std::size_t>(y) * width_ + x;
            const int8_t current = cells_[i];
            const bool live = current > 0 ? (neighbours == 2 || neighbours == 3) : neighbours == 3;
            int8_t next;
            if (live) {
                next = current > 0 ? static_cast<int8_t>(current < kMaxAge ? current + 1 : kMaxAge) : 1;
                ++population_;
            } else {
                next = current > 0 ? 0 : static_cast<int8_t>(current > kMaxDeadAge ? current - 1 : kMaxDeadAge);
            }
            next_[i] = next;
        }
    }
    cells_.swap(next_);
    ++generation_;

    const uint64_t hash = live_hash();
    // Matching the previous or the one before covers still lifes and blinkers.
    if (population_ == 0 || hash == hashes_[0] || hash == hashes_[1])
        ++stale_steps_;
    else
        stale_steps_ = 0;
    hashes_[1] = hashes_[0];
    hashes_[0] = hash;
}
