// SPDX-License-Identifier: MIT

#include "../main/ui/model/slime_model.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace slime;

namespace {

constexpr int kW = 310;
constexpr int kH = 130;

void test_determinism()
{
    Model a(kW, kH, 3500, 1234u);
    Model b(kW, kH, 3500, 1234u);
    Model c(kW, kH, 3500, 1235u);
    for (int i = 0; i < 50; ++i) {
        a.step();
        b.step();
        c.step();
    }
    assert(a.checksum() == b.checksum());
    assert(a.field() == b.field());
    assert(a.checksum() != c.checksum());

    // Reseeding with the same seed and pattern replays the same run.
    a.reseed(77u, Pattern::Ring);
    b.reseed(77u, Pattern::Ring);
    for (int i = 0; i < 50; ++i) {
        a.step();
        b.step();
    }
    assert(a.checksum() == b.checksum());
}

void test_bounds()
{
    for (int pattern = 0; pattern < kPatternCount; ++pattern) {
        Model m(kW, kH, 2000, 9u + static_cast<uint32_t>(pattern));
        m.reseed(42u, static_cast<Pattern>(pattern));
        assert(m.agents_in_bounds());
        assert(m.total() == 0.0);
        for (int i = 0; i < 1500; ++i) m.step();
        assert(m.agents_in_bounds());
        assert(m.total() > 0.0);
    }
    // A small odd-sized field exercises the wrap far more often.
    Model small(17, 11, 300, 5u);
    for (int i = 0; i < 5000; ++i) small.step();
    assert(small.agents_in_bounds());
}

void test_decay_to_zero()
{
    Model m(kW, kH, 3000, 3u);
    for (int i = 0; i < 100; ++i) m.step();
    assert(m.total() > 100.0);
    m.set_agent_count(0);
    assert(m.agent_count() == 0);
    double previous = m.total();
    for (int i = 0; i < 400; ++i) {
        m.step();
        const double now = m.total();
        assert(now <= previous);
        previous = now;
    }
    assert(m.total() == 0.0);
    for (float v : m.field()) assert(v == 0.0f);
}

void test_deposit_bounded()
{
    for (const Species &species : kSpecies) {
        Model m(kW, kH, kMaxAgents, 11u);
        m.set_params(species.params);
        // Each step adds at most agents * deposit and then scales the field by
        // the decay, so the total cannot exceed the geometric series limit.
        const double limit = kMaxAgents * static_cast<double>(species.params.deposit) * species.params.decay /
                             (1.0 - species.params.decay);
        for (int i = 0; i < 300; ++i) {
            m.step();
            assert(m.total() <= limit * 1.001);
        }
        const float peak = *std::max_element(m.field().begin(), m.field().end());
        assert(peak <= kMaxTrail);
        assert(peak > 0.0f);
    }
}

void test_presets()
{
    std::vector<uint32_t> sums;
    for (std::size_t i = 0; i < kSpecies.size(); ++i) {
        Model m(kW, kH, 3500, 2024u);
        m.set_params(kSpecies[i].params);
        assert(m.params().sensor_angle_deg == kSpecies[i].params.sensor_angle_deg);
        assert(m.params().sensor_dist == kSpecies[i].params.sensor_dist);
        for (int s = 0; s < 60; ++s) m.step();
        assert(m.agents_in_bounds());
        assert(m.total() > 0.0);
        sums.push_back(m.checksum());
        assert(std::strlen(kSpecies[i].name) > 0);
    }
    // Same seed, different species: every preset produces a different field.
    std::sort(sums.begin(), sums.end());
    assert(std::adjacent_find(sums.begin(), sums.end()) == sums.end());
}

void test_controls()
{
    assert(clamp_agents(0) == kMinAgents);
    assert(clamp_agents(1000000) == kMaxAgents);
    assert(clamp_agents(3500) == 3500);
    assert(clamp_sensor_angle(0) == kMinSensorAngle);
    assert(clamp_sensor_angle(400) == kMaxSensorAngle);

    Model m(kW, kH, 1000, 8u);
    m.set_agent_count(4000);
    assert(m.agent_count() == 4000);
    assert(m.agents_in_bounds());
    m.set_agent_count(1500);
    assert(m.agent_count() == 1500);
    m.set_sensor_angle(1000);
    assert(m.params().sensor_angle_deg == kMaxSensorAngle);
    for (int i = 0; i < 20; ++i) m.step();
    assert(m.agents_in_bounds());

    // The disc starts inside the middle of the field, the ring outside it.
    m.reseed(5u, Pattern::Disc);
    m.step();
    std::vector<uint8_t> levels(static_cast<std::size_t>(kW) * kH);
    m.levels(levels.data());
    assert(levels[0] == 0);
    assert(levels[static_cast<std::size_t>(kH / 2) * kW + 10] == 0);
    int lit = 0;
    for (uint8_t v : levels) {
        assert(v == 0 || (v >= 12 && v <= 63));
        if (v) ++lit;
    }
    assert(lit > 0);
}

void test_palette()
{
    assert(palette_rgb(0) == 0x000000u);
    assert(palette_rgb(12) == 0x000000u);
    assert(palette_rgb(28) == 0x13261Bu);
    assert(palette_rgb(46) == 0x3DDC84u);
    assert(palette_rgb(63) == 0xFFFFFFu);
    assert(palette_rgb(-5) == 0x000000u);
    assert(palette_rgb(99) == 0xFFFFFFu);
    // Palette 0 is the green scale; the others are coloured ramps that start
    // black, end at their top stop and never repeat a name.
    assert(std::string(palette_name(0)) == "phosphor");
    assert(std::string(palette_name(1)) == "bioluminescence");
    assert(std::string(palette_name(kPaletteCount)) == "phosphor");
    assert(std::string(palette_name(-1)) == "abyss");
    for (int p = 0; p < kPaletteCount; ++p) {
        assert(palette_rgb(p, 0) == 0x000000u);
        assert(palette_rgb(p, 11) == 0x000000u);
        assert(palette_rgb(p, 12) != 0x000000u || p == 0);
        for (int q = p + 1; q < kPaletteCount; ++q) assert(std::string(palette_name(p)) != palette_name(q));
    }
    assert(palette_rgb(0, 46) == palette_rgb(46));
    assert(palette_rgb(1, 63) == 0xFFFCECu);
    assert(palette_rgb(4, 63) == 0xFFFFFFu);
    assert(palette_rgb(7, 63) == 0xFFFFF8u);
    assert(palette_index(0.0f) == 0);
    assert(palette_index(0.04f) == 0);
    assert(palette_index(0.1f) == 15);
    assert(palette_index(1.0f) == 42);
    assert(palette_index(kMaxTrail) == 63);
}

void test_performance()
{
    Model m(kW, kH, 4000, 99u);
    std::vector<uint8_t> levels(static_cast<std::size_t>(kW) * kH);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        m.step();
        m.levels(levels.data());
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("slime: 100 steps of %dx%d with %d agents took %.1f ms (%.2f ms per step)\n", kW, kH,
                m.agent_count(), ms, ms / 100.0);
    assert(ms < 500.0);
}

} // namespace

int main()
{
    test_determinism();
    test_bounds();
    test_decay_to_zero();
    test_deposit_bounded();
    test_presets();
    test_controls();
    test_palette();
    test_performance();
    std::printf("test_slime_model: ok\n");
    return 0;
}
