/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Conway's Game of Life on a wrapping grid, with per-cell age so the lock
// screen can colour newborn, settled and recently dead cells differently.
class LifeGrid
{
public:
    // Age encoding: > 0 alive for that many generations (capped), <= 0 dead
    // for -age generations (capped), so trails can fade out.
    static constexpr int8_t kMaxAge = 24;
    static constexpr int8_t kMaxDeadAge = -8;

    LifeGrid(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    bool alive(int x, int y) const { return age(x, y) > 0; }
    int8_t age(int x, int y) const { return cells_[index(x, y)]; }
    std::size_t population() const { return population_; }
    uint64_t generation() const { return generation_; }

    void clear();
    void set(int x, int y, bool on);
    // Fills about density_percent of the cells from a xorshift seed.
    void randomize(uint32_t seed, int density_percent);
    // Places a glider with its top-left corner at (x, y), heading down-right.
    void glider(int x, int y);
    void step();

    // True when the pattern has died out or settled into a still life or a
    // period-2 oscillator for several generations.
    bool stagnant() const { return stale_steps_ >= 6; }

private:
    std::size_t index(int x, int y) const;
    uint64_t live_hash() const;

    int width_;
    int height_;
    std::vector<int8_t> cells_;
    std::vector<int8_t> next_;
    std::size_t population_ = 0;
    uint64_t generation_ = 0;
    uint64_t hashes_[2] = {0, 0};
    int stale_steps_ = 0;
};
