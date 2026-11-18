/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Physarum (slime mould) agent simulation: agents sense a trail field at three
// points ahead, turn toward the strongest, move one cell and deposit; the
// field then diffuses with a 3x3 blur and decays. The field wraps at the edges.
namespace slime {

inline constexpr int kHeadings = 256;
inline constexpr float kMaxTrail = 3.0f;
inline constexpr int kMinAgents = 500;
inline constexpr int kMaxAgents = 6000;
inline constexpr int kAgentStep = 500;
inline constexpr int kMinSensorAngle = 6;
inline constexpr int kMaxSensorAngle = 90;
inline constexpr int kSensorAngleStep = 4;

struct Params {
    int sensor_angle_deg;
    int sensor_dist;
    int turn_deg;
    float deposit;
    float decay;
};

struct Species {
    const char *name;
    Params params;
};

inline constexpr std::array<Species, 6> kSpecies = {{
    {"physarum", {28, 9, 20, 0.30f, 0.90f}},
    {"veins", {22, 14, 12, 0.45f, 0.94f}},
    {"lattice", {58, 6, 44, 0.35f, 0.88f}},
    {"coral", {40, 4, 60, 0.60f, 0.80f}},
    {"smoke", {14, 18, 6, 0.20f, 0.96f}},
    {"storm", {78, 11, 30, 0.50f, 0.85f}},
}};

enum class Pattern { Scatter, Disc, Ring };
inline constexpr int kPatternCount = 3;
const char *pattern_name(Pattern pattern);

int clamp_agents(int count);
int clamp_sensor_angle(int degrees);

// Green-through-white scale, the same stops as the SDR waterfall. index 0..63.
uint32_t palette_rgb(int index);
// Palette index for a trail value: 0 for an empty cell, otherwise 12..63.
uint8_t palette_index(float trail);

class Model
{
public:
    Model(int width, int height, int agents, uint32_t seed);

    int width() const { return width_; }
    int height() const { return height_; }
    int agent_count() const { return static_cast<int>(agents_.size()); }
    const Params &params() const { return params_; }
    const std::vector<float> &field() const { return field_; }

    void set_params(const Params &params);
    void set_sensor_angle(int degrees);
    // Grows with freshly scattered agents or drops agents from the end.
    void set_agent_count(int count);
    // Clears the field and places the current number of agents again.
    void reseed(uint32_t seed, Pattern pattern);

    void step();

    // One palette index per cell, row by row; out must hold width * height.
    void levels(uint8_t *out) const;

    double total() const;
    uint32_t checksum() const;
    bool agents_in_bounds() const;

private:
    struct Agent {
        float x;
        float y;
        uint8_t heading;
    };

    int width_;
    int height_;
    Params params_{};
    uint32_t rng_;
    std::vector<Agent> agents_;
    std::vector<float> field_;
    std::vector<float> scratch_;
    std::array<int16_t, kHeadings> sense_dx_{};
    std::array<int16_t, kHeadings> sense_dy_{};
    int angle_units_ = 0;
    int turn_units_ = 0;

    uint32_t next_random();
    float random_unit();
    Agent scatter_agent();
    void rebuild_tables();
    void move_agents();
    void diffuse();
};

} // namespace slime
