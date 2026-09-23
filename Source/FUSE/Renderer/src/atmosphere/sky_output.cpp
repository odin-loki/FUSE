#include <fuse/renderer/atmosphere/sky_output.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

/// PCG-style integer hash — GPU-friendly (32-bit integer ops only).
u32 hash32(u32 value) {
    const u32 state = value * 747796405u + 2891336453u;
    const u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

f32 hashToUnit(u32 value) {
    // 24 high-quality bits -> [0, 1).
    return static_cast<f32>(hash32(value) >> 8u) * (1.f / 16777216.f);
}

} // namespace

f32 sky_dither_tpdf(u32 x, u32 y, u32 frame) {
    const u32 seed = hash32(x + hash32(y + hash32(frame)));
    const f32 u0 = hashToUnit(seed);
    const f32 u1 = hashToUnit(seed ^ 0x9E3779B9u);
    return u0 + u1 - 1.f;
}

u32 sky_quantize_unorm(f32 value, u32 bits, f32 dither_lsb) {
    const u32 clamped_bits = std::max(1u, std::min(16u, bits));
    const f32 max_code = static_cast<f32>((1u << clamped_bits) - 1u);
    const f32 scaled = std::max(0.f, std::min(1.f, value)) * max_code + dither_lsb;
    const f32 rounded = std::floor(scaled + 0.5f);
    return static_cast<u32>(std::max(0.f, std::min(max_code, rounded)));
}

} // namespace fuse::renderer
