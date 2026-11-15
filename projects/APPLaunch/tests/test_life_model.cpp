// SPDX-License-Identifier: MIT

#include "../main/ui/model/life_model.hpp"

#include <cassert>

int main()
{
    // Blinker: period 2, flagged stagnant after a few generations.
    LifeGrid blinker(8, 8);
    blinker.set(3, 2, true);
    blinker.set(3, 3, true);
    blinker.set(3, 4, true);
    blinker.step();
    assert(blinker.alive(2, 3) && blinker.alive(3, 3) && blinker.alive(4, 3));
    assert(!blinker.alive(3, 2) && !blinker.alive(3, 4));
    assert(blinker.population() == 3);
    assert(blinker.age(3, 3) == 2);
    assert(blinker.age(2, 3) == 1);
    assert(blinker.age(3, 2) == 0);
    for (int i = 0; i < 8; ++i) blinker.step();
    assert(blinker.stagnant());

    // Block: still life.
    LifeGrid block(6, 6);
    block.set(1, 1, true);
    block.set(2, 1, true);
    block.set(1, 2, true);
    block.set(2, 2, true);
    for (int i = 0; i < 3; ++i) block.step();
    assert(block.population() == 4 && block.alive(2, 2));
    assert(block.age(1, 1) == 4);

    // Glider crosses the wrapping edge and keeps its five cells.
    LifeGrid glider(10, 10);
    glider.glider(7, 7);
    for (int i = 0; i < 40; ++i) glider.step();
    assert(glider.population() == 5);
    assert(!glider.stagnant());

    // Lone cell dies; its trail ages towards the floor.
    LifeGrid lone(5, 5);
    lone.set(2, 2, true);
    lone.step();
    assert(lone.population() == 0 && lone.age(2, 2) == 0);
    for (int i = 0; i < 20; ++i) lone.step();
    assert(lone.age(2, 2) == LifeGrid::kMaxDeadAge);
    assert(lone.stagnant());

    // Random fill is deterministic per seed and close to the density.
    LifeGrid a(80, 37), b(80, 37);
    a.randomize(42, 30);
    b.randomize(42, 30);
    assert(a.population() == b.population());
    assert(a.population() > 80 * 37 * 20 / 100 && a.population() < 80 * 37 * 40 / 100);
    return 0;
}
