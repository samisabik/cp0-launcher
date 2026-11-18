/*
 * SPDX-License-Identifier: MIT
 */

#include "slime_model.hpp"

#include <algorithm>
#include <cmath>

namespace slime {

namespace {

constexpr double kTau = 6.283185307179586;
// Subtracted after the decay so an idle field reaches exactly zero instead of
// lingering in denormal floats.
constexpr float kFloor = 1e-4f;
constexpr float kVisible = 0.05f;

struct Directions {
    std::array<float, kHeadings> cos{};
    std::array<float, kHeadings> sin{};
};

const Directions &directions()
{
    static const Directions table = [] {
        Directions d;
        for (int i = 0; i < kHeadings; ++i) {
            d.cos[i] = static_cast<float>(std::cos(kTau * i / kHeadings));
            d.sin[i] = static_cast<float>(std::sin(kTau * i / kHeadings));
        }
        return d;
    }();
    return table;
}

int degrees_to_units(int degrees)
{
    return (degrees * kHeadings + 180) / 360;
}

uint8_t heading_toward(float dx, float dy)
{
    const double angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
    return static_cast<uint8_t>(static_cast<int>(std::lround(angle / kTau * kHeadings)) & (kHeadings - 1));
}

} // namespace

const char *pattern_name(Pattern pattern)
{
    switch (pattern) {
    case Pattern::Scatter: return "scatter";
    case Pattern::Disc: return "disc";
    case Pattern::Ring: return "ring";
    }
    return "";
}

int clamp_agents(int count)
{
    return std::clamp(count, kMinAgents, kMaxAgents);
}

int clamp_sensor_angle(int degrees)
{
    return std::clamp(degrees, kMinSensorAngle, kMaxSensorAngle);
}

uint32_t palette_rgb(int index)
{
    struct Stop { int at, r, g, b; };
    static constexpr Stop kStops[] = {
        {0, 0, 0, 0}, {12, 0, 0, 0}, {28, 0x13, 0x26, 0x1B}, {46, 0x3D, 0xDC, 0x84}, {63, 0xFF, 0xFF, 0xFF}};
    index = std::clamp(index, 0, 63);
    for (std::size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
        const Stop &a = kStops[i - 1];
        const Stop &b = kStops[i];
        if (index <= b.at) {
            const int span = b.at - a.at;
            const int t = index - a.at;
            return static_cast<uint32_t>(((a.r + (b.r - a.r) * t / span) << 16) |
                                         ((a.g + (b.g - a.g) * t / span) << 8) | (a.b + (b.b - a.b) * t / span));
        }
    }
    return 0xFFFFFF;
}

namespace {

struct RampStop {
    int at, r, g, b;
};
struct Ramp {
    const char *name;
    const RampStop *stops;
    int count;
};

// Front-loaded on purpose: most of a field sits at low intensity, so the
// first third of each ramp covers the widest colour span.
constexpr RampStop kBio[] = {{0, 0, 0, 0},        {14, 8, 0, 26},      {40, 46, 2, 86},     {86, 146, 10, 158},
                             {140, 236, 62, 128}, {196, 255, 150, 96}, {230, 255, 216, 150}, {255, 255, 252, 236}};
constexpr RampStop kCyan[] = {{0, 0, 0, 0},        {14, 0, 6, 30},       {40, 0, 38, 92},      {86, 0, 104, 168},
                              {140, 30, 176, 216}, {196, 120, 232, 240}, {230, 208, 250, 255}, {255, 255, 255, 255}};
constexpr RampStop kEmber[] = {{0, 0, 0, 0},        {14, 22, 2, 0},      {40, 84, 10, 4},      {86, 176, 40, 6},
                               {140, 240, 108, 16}, {196, 255, 178, 56}, {230, 255, 228, 140}, {255, 255, 255, 238}};
constexpr RampStop kSpectral[] = {{0, 0, 0, 0},        {12, 26, 0, 48},     {44, 20, 32, 150},
                                  {84, 0, 148, 156},   {124, 108, 216, 70}, {166, 250, 210, 44},
                                  {206, 252, 92, 56},  {236, 255, 176, 200}, {255, 255, 255, 255}};
constexpr RampStop kBone[] = {{0, 0, 0, 0},         {16, 10, 16, 18},     {46, 36, 56, 54},     {96, 92, 108, 98},
                              {150, 156, 164, 146}, {200, 208, 208, 188}, {236, 240, 238, 224}, {255, 255, 255, 250}};
constexpr RampStop kUv[] = {{0, 0, 0, 0},        {14, 12, 0, 34},      {42, 46, 0, 104},     {88, 106, 18, 196},
                            {140, 176, 70, 246}, {192, 224, 146, 255}, {230, 246, 208, 255}, {255, 255, 250, 255}};
constexpr RampStop kAbyss[] = {{0, 1, 3, 9},        {14, 3, 12, 32},      {42, 0, 44, 72},      {88, 0, 98, 108},
                               {140, 36, 168, 150}, {192, 132, 224, 186}, {230, 214, 250, 230}, {255, 255, 255, 248}};

constexpr Ramp kRamps[kPaletteCount - 1] = {
    {"bioluminescence", kBio, 8}, {"cyanotype", kCyan, 8}, {"ember", kEmber, 8}, {"spectral", kSpectral, 9},
    {"bone", kBone, 8},           {"ultraviolet", kUv, 8}, {"abyss", kAbyss, 8},
};

int wrap_palette(int palette)
{
    return ((palette % kPaletteCount) + kPaletteCount) % kPaletteCount;
}

} // namespace

const char *palette_name(int palette)
{
    palette = wrap_palette(palette);
    return palette == 0 ? "phosphor" : kRamps[palette - 1].name;
}

uint32_t palette_rgb(int palette, int index)
{
    palette = wrap_palette(palette);
    if (palette == 0) return palette_rgb(index);
    index = std::clamp(index, 0, 63);
    if (index < 12) return 0x000000;
    // Visible trails use indices 12..63; spread them over the ramp from just
    // above black to its top.
    const int at = 14 + (index - 12) * 241 / 51;
    const Ramp &ramp = kRamps[palette - 1];
    for (int i = 1; i < ramp.count; ++i) {
        const RampStop &a = ramp.stops[i - 1];
        const RampStop &b = ramp.stops[i];
        if (at <= b.at) {
            const int span = b.at - a.at;
            const int t = at - a.at;
            return static_cast<uint32_t>(((a.r + (b.r - a.r) * t / span) << 16) |
                                         ((a.g + (b.g - a.g) * t / span) << 8) | (a.b + (b.b - a.b) * t / span));
        }
    }
    const RampStop &top = ramp.stops[ramp.count - 1];
    return static_cast<uint32_t>((top.r << 16) | (top.g << 8) | top.b);
}

uint8_t palette_index(float trail)
{
    if (!(trail > kVisible)) return 0;
    const int index = 12 + static_cast<int>(trail * 30.0f);
    return static_cast<uint8_t>(index > 63 ? 63 : index);
}

Model::Model(int width, int height, int agents, uint32_t seed)
    : width_(std::max(width, 8)), height_(std::max(height, 8)), rng_(seed ? seed : 1u)
{
    field_.assign(static_cast<std::size_t>(width_) * height_, 0.0f);
    scratch_ = field_;
    agents_.resize(static_cast<std::size_t>(std::max(agents, 0)));
    set_params(kSpecies[0].params);
    reseed(seed, Pattern::Scatter);
}

uint32_t Model::next_random()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

float Model::random_unit()
{
    return static_cast<float>(next_random() >> 8) * (1.0f / 16777216.0f);
}

Model::Agent Model::scatter_agent()
{
    Agent a;
    a.x = random_unit() * static_cast<float>(width_);
    a.y = random_unit() * static_cast<float>(height_);
    a.heading = static_cast<uint8_t>(next_random() >> 24);
    return a;
}

void Model::set_params(const Params &params)
{
    params_ = params;
    params_.sensor_angle_deg = clamp_sensor_angle(params.sensor_angle_deg);
    // The sensor must stay within one wrap of the field.
    params_.sensor_dist = std::clamp(params.sensor_dist, 1, std::min(width_, height_) - 1);
    params_.turn_deg = std::clamp(params.turn_deg, 1, 120);
    params_.deposit = std::clamp(params.deposit, 0.0f, kMaxTrail);
    params_.decay = std::clamp(params.decay, 0.0f, 0.999f);
    rebuild_tables();
}

void Model::set_sensor_angle(int degrees)
{
    Params next = params_;
    next.sensor_angle_deg = degrees;
    set_params(next);
}

void Model::rebuild_tables()
{
    const Directions &dir = directions();
    for (int i = 0; i < kHeadings; ++i) {
        sense_dx_[i] = static_cast<int16_t>(std::lround(dir.cos[i] * static_cast<float>(params_.sensor_dist)));
        sense_dy_[i] = static_cast<int16_t>(std::lround(dir.sin[i] * static_cast<float>(params_.sensor_dist)));
    }
    angle_units_ = std::max(1, degrees_to_units(params_.sensor_angle_deg));
    turn_units_ = std::max(1, degrees_to_units(params_.turn_deg));
}

void Model::set_agent_count(int count)
{
    const std::size_t next = static_cast<std::size_t>(std::max(count, 0));
    const std::size_t old = agents_.size();
    agents_.resize(next);
    for (std::size_t i = old; i < next; ++i) agents_[i] = scatter_agent();
}

void Model::reseed(uint32_t seed, Pattern pattern)
{
    rng_ = seed ? seed : 1u;
    std::fill(field_.begin(), field_.end(), 0.0f);
    const float cx = static_cast<float>(width_) * 0.5f;
    const float cy = static_cast<float>(height_) * 0.5f;
    const float radius = static_cast<float>(std::min(width_, height_)) * 0.42f;
    const Directions &dir = directions();
    for (Agent &a : agents_) {
        if (pattern == Pattern::Scatter) {
            a = scatter_agent();
            continue;
        }
        const uint8_t spoke = static_cast<uint8_t>(next_random() >> 24);
        // The square root spreads agents evenly over the disc area.
        const float r = pattern == Pattern::Disc ? radius * 0.45f * std::sqrt(random_unit())
                                                 : radius * (0.94f + 0.06f * random_unit());
        a.x = cx + dir.cos[spoke] * r;
        a.y = cy + dir.sin[spoke] * r;
        a.heading = pattern == Pattern::Disc ? spoke : heading_toward(cx - a.x, cy - a.y);
    }
}

void Model::move_agents()
{
    const Directions &dir = directions();
    const int w = width_;
    const int h = height_;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    float *field = field_.data();
    const float deposit = params_.deposit;
    const int angle = angle_units_;
    const int turn = turn_units_;
    const int16_t *sdx = sense_dx_.data();
    const int16_t *sdy = sense_dy_.data();

    // The sensor offset is shorter than the field, so one conditional add or
    // subtract wraps it.
    auto sense = [=](int ix, int iy, int heading) {
        int x = ix + sdx[heading];
        int y = iy + sdy[heading];
        if (x < 0) x += w;
        else if (x >= w) x -= w;
        if (y < 0) y += h;
        else if (y >= h) y -= h;
        return field[y * w + x];
    };

    for (Agent &a : agents_) {
        const int ix = static_cast<int>(a.x);
        const int iy = static_cast<int>(a.y);
        int heading = a.heading;
        const float ahead = sense(ix, iy, heading);
        const float left = sense(ix, iy, (heading - angle) & (kHeadings - 1));
        const float right = sense(ix, iy, (heading + angle) & (kHeadings - 1));
        if (ahead < left || ahead < right) {
            if (left > right) {
                heading -= turn;
            } else if (right > left) {
                heading += turn;
            } else {
                const int span = 2 * turn + 1;
                heading += static_cast<int>(next_random() % static_cast<uint32_t>(span)) - turn;
            }
            heading &= kHeadings - 1;
            a.heading = static_cast<uint8_t>(heading);
        }
        float x = a.x + dir.cos[heading];
        float y = a.y + dir.sin[heading];
        if (x < 0.0f) x += fw;
        else if (x >= fw) x -= fw;
        if (y < 0.0f) y += fh;
        else if (y >= fh) y -= fh;
        // Adding the size to a tiny negative value can round up to the size itself.
        int cx = static_cast<int>(x);
        int cy = static_cast<int>(y);
        if (cx >= w) {
            cx = w - 1;
            x = std::nextafter(fw, 0.0f);
        }
        if (cy >= h) {
            cy = h - 1;
            y = std::nextafter(fh, 0.0f);
        }
        a.x = x;
        a.y = y;
        float &cell = field[cy * w + cx];
        const float value = cell + deposit;
        cell = value > kMaxTrail ? kMaxTrail : value;
    }
}

void Model::diffuse()
{
    const int w = width_;
    const int h = height_;
    const float scale = params_.decay / 9.0f;
    const float *src = field_.data();
    float *dst = scratch_.data();

    auto wrapped = [&](int x, int y) {
        float sum = 0.0f;
        for (int dy = -1; dy <= 1; ++dy) {
            const int yy = (y + dy + h) % h;
            for (int dx = -1; dx <= 1; ++dx) sum += src[yy * w + (x + dx + w) % w];
        }
        const float v = sum * scale - kFloor;
        return v > 0.0f ? v : 0.0f;
    };

    for (int y = 1; y < h - 1; ++y) {
        const float *up = src + static_cast<std::size_t>(y - 1) * w;
        const float *mid = up + w;
        const float *down = mid + w;
        float *out = dst + static_cast<std::size_t>(y) * w;
        for (int x = 1; x < w - 1; ++x) {
            const float sum = up[x - 1] + up[x] + up[x + 1] + mid[x - 1] + mid[x] + mid[x + 1] + down[x - 1] +
                              down[x] + down[x + 1];
            const float v = sum * scale - kFloor;
            out[x] = v > 0.0f ? v : 0.0f;
        }
        out[0] = wrapped(0, y);
        out[w - 1] = wrapped(w - 1, y);
    }
    for (int x = 0; x < w; ++x) {
        dst[x] = wrapped(x, 0);
        dst[static_cast<std::size_t>(h - 1) * w + x] = wrapped(x, h - 1);
    }
    field_.swap(scratch_);
}

void Model::step()
{
    move_agents();
    diffuse();
}

void Model::levels(uint8_t *out) const
{
    const std::size_t count = field_.size();
    const float *src = field_.data();
    for (std::size_t i = 0; i < count; ++i) out[i] = palette_index(src[i]);
}

double Model::total() const
{
    double sum = 0.0;
    for (float v : field_) sum += v;
    return sum;
}

uint32_t Model::checksum() const
{
    // FNV-1a over the quantised field.
    uint32_t hash = 2166136261u;
    for (float v : field_) {
        const uint32_t q = static_cast<uint32_t>(v * 4096.0f);
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (q >> shift) & 0xFFu;
            hash *= 16777619u;
        }
    }
    return hash;
}

bool Model::agents_in_bounds() const
{
    for (const Agent &a : agents_)
        if (!(a.x >= 0.0f && a.x < static_cast<float>(width_) && a.y >= 0.0f && a.y < static_cast<float>(height_)))
            return false;
    return true;
}

} // namespace slime
