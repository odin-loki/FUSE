/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix
// src/dxvk/shaders/rtx/pass/particles/particle_system_{spawn,evolve,generate_geometry}.comp.slang@0867d3c,
// particle_system_binding_indices.h@0867d3c (GpuParticle) and
// src/dxvk/shaders/rtx/utility/{noise,procedural_noise,sampling,
// math,packing}.slangh@0867d3c
// Portions Copyright (c) 2023-2024, NVIDIA CORPORATION. All rights reserved.
// (src/dxvk/shaders/rtx/utility/noise.slangh@0867d3c),
// same MIT licence.

// FUSE Relight RL-3.6: the particle simulation as single-source kernels (docs/compute-kernels.md). These bodies are
// the CPU reference of the three GPU passes and run on kernel::Backend::CpuReference / CpuParallel; the GLSL twin
// (shaders/particle_sim.comp + particle_common.glsl) mirrors them expression for expression.
//
//   relight_particle_spawn      item kernel over the frame's spawn count: a dead or sleeping ring slot is respawned
//                               on a random triangle of its emitter (spawn context), with the initial velocity from
//                               the emitter's motion and a cone around the triangle normal.
//   relight_particle_evolve     item kernel over the simulated range (the live ring minus this frame's spawns):
//                               turbulence (curl of value noise), gravity, attractor, drag, per-axis restriction,
//                               max-velocity clamp from the animation table, integration, rotation; time to live
//                               decreases and a particle past its conservative death time is retired and counted
//                               (global_atomic_add on the system's death counter).
//   relight_particle_billboard  item kernel over maxNumParticles: live particles become camera-facing quads (4 vertices,
//                               8 with a motion trail) at [i * vpp, (i + 1) * vpp) of the system's vertex range; slots
//                               past the live count (and culled particles) are written as zero vertices, so the draw of
//                               a conservative count never shows stale geometry.
//
// Determinism. The random numbers are integer hashes (upstream uintHash + unorm23ToFloat, exact on every backend);
// the state that decides life and death (time to live, the random seed, the state flag, the counters) only goes
// through correctly rounded f32 add / sub / mul (no contraction: the GLSL twin marks them `precise`), so it is
// bit-identical between the CPU backends and the GPU. Positions, velocities, colours and billboards also use sqrt and
// division (correctly rounded on the CPU; Vulkan allows 2.5 ulp) and a portable sine (psin / pcos: the platform sin /
// cos differ between C runtimes and GPUs); the GLSL twin marks the float chains `precise` (no contraction). On
// Lavapipe the GPU output is bit-identical to the CPU reference; other GPUs are held to the gate's 1e-4 tolerance.
//
// Modifications (FUSE):
//   * C++ single-source bodies (FUSE_HOST_DEVICE) instead of Slang; f32 throughout (upstream: half rotation, time to
//     live, uv and max velocity);
//   * the evolve step runs for every live, non-sleeping particle (upstream skips it only when a whole wave is asleep,
//     so a sleeping particle's motion depended on the wave size);
//   * the random stream hashes (slot, frame, system seed) (upstream: (slot, frame), identical across systems);
//   * value noise hashes its lattice (upstream: a 64^3 RGBA8 look-up texture sampled with trilinear filtering; the
//     lattice here is the same 64-periodic trilinear field with hashed texels);
//   * the animation table is f32 and bilinearly sampled in the kernel (upstream: an RGBA16F texture);
//   * the emission-cone axis is normalized (upstream passes the unnormalized transformed face normal);
//   * scene collisions need the previous frame's G-buffer, which Relight does not produce yet (RL-5.x): the
//     collision flag is carried but the kernels do not collide;
//   * rotation wraps with a truncated remainder written out (Slang `%`), not GLSL mod();
//   * sin / cos are a portable polynomial (psin / pcos) instead of the platform's, so every backend rounds alike;
//   * culled and unused billboard slots are written as all-zero vertices (upstream: position only).
#pragma once

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/relight/particles/particle_types.hpp>
#include <fuse/types.hpp>

#include <bit>
#include <cmath>

namespace fuse::relight::particles::kernels {

inline constexpr const char* kSpawnName = "relight_particle_spawn";
inline constexpr const char* kEvolveName = "relight_particle_evolve";
inline constexpr const char* kBillboardName = "relight_particle_billboard";
inline constexpr kernel::Dim3 kWorkgroup{kWorkgroupSize, 1u, 1u};

// ---- scalar helpers -----------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline f32 asFloat(u32 bits) { return std::bit_cast<f32>(bits); }
FUSE_HOST_DEVICE inline u32 asUint(f32 f) { return std::bit_cast<u32>(f); }
FUSE_HOST_DEVICE inline f32 minf(f32 a, f32 b) { return b < a ? b : a; }
FUSE_HOST_DEVICE inline f32 maxf(f32 a, f32 b) { return a < b ? b : a; }
FUSE_HOST_DEVICE inline f32 clampf(f32 x, f32 lo, f32 hi) { return minf(maxf(x, lo), hi); }
FUSE_HOST_DEVICE inline f32 saturate(f32 x) { return clampf(x, 0.f, 1.f); }
/// a + (b - a) * t (the GLSL twin's `mixp`, precise).
FUSE_HOST_DEVICE inline f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

/// 1/30 as f32 (upstream kMinimumParticleLife), spelled as bits so both sides use the same value.
FUSE_HOST_DEVICE inline f32 minimumParticleLife() { return asFloat(0x3D088889u); }
inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTwoPi = 6.28318530717958647692f;
inline constexpr f32 kDegToRad = 0.01745329251994329577f;
inline constexpr f32 kInv255 = 1.f / 255.f;

// ---- portable sine / cosine ---------------------------------------------------------------------------------------

/// sin(x) from correctly rounded f32 add / sub / mul only (Cody-Waite reduction by 2 pi in two parts, reflection to
/// [-pi/2, pi/2], degree-11 Taylor polynomial; |error| < 1e-6 for |x| < 64), so the CPU backends (glibc or the MinGW
/// runtime) and the GPU (whose sin / cos are only specified to 2^-11 absolute; Lavapipe's are ~1e-6) compute the same
/// bits. The GLSL twin marks every step `precise`.
FUSE_HOST_DEVICE inline f32 psin(f32 x) {
    const f32 k = std::floor(x * asFloat(0x3E22F983u) + 0.5f);
    f32 r = x - k * asFloat(0x40C90FDBu);
    r = r - k * asFloat(0xB43BBD2Eu);
    const f32 halfPi = asFloat(0x3FC90FDBu);
    const f32 pi = asFloat(0x40490FDBu);
    if (r > halfPi) {
        r = pi - r;
    } else if (r < -halfPi) {
        r = -pi - r;
    }
    const f32 r2 = r * r;
    const f32 p = asFloat(0xB2D7322Bu);
    const f32 p3 = asFloat(0x3638EF1Du) + r2 * p;
    const f32 p2 = asFloat(0xB9500D01u) + r2 * p3;
    const f32 p1 = asFloat(0x3C088889u) + r2 * p2;
    const f32 p0 = asFloat(0xBE2AAAABu) + r2 * p1;
    return r * (1.f + r2 * p0);
}
FUSE_HOST_DEVICE inline f32 pcos(f32 x) { return psin(x + asFloat(0x3FC90FDBu)); }

// ---- 3-vectors ----------------------------------------------------------------------------------------------------

struct F3 {
    f32 x, y, z;
};
FUSE_HOST_DEVICE inline F3 f3(f32 x, f32 y, f32 z) { return F3{x, y, z}; }
FUSE_HOST_DEVICE inline F3 f3(const f32* v) { return F3{v[0], v[1], v[2]}; }
FUSE_HOST_DEVICE inline void store(f32* out, const F3& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}
FUSE_HOST_DEVICE inline F3 operator+(const F3& a, const F3& b) { return F3{a.x + b.x, a.y + b.y, a.z + b.z}; }
FUSE_HOST_DEVICE inline F3 operator-(const F3& a, const F3& b) { return F3{a.x - b.x, a.y - b.y, a.z - b.z}; }
FUSE_HOST_DEVICE inline F3 operator*(const F3& a, f32 s) { return F3{a.x * s, a.y * s, a.z * s}; }
FUSE_HOST_DEVICE inline F3 operator*(const F3& a, const F3& b) { return F3{a.x * b.x, a.y * b.y, a.z * b.z}; }
FUSE_HOST_DEVICE inline f32 dot(const F3& a, const F3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
FUSE_HOST_DEVICE inline F3 cross(const F3& a, const F3& b) {
    return F3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
FUSE_HOST_DEVICE inline f32 length(const F3& a) { return std::sqrt(dot(a, a)); }
FUSE_HOST_DEVICE inline F3 normalize(const F3& a) { return a * (1.f / std::sqrt(dot(a, a))); }
/// upstream safeNormalize: `fallback` for a (near) zero vector.
FUSE_HOST_DEVICE inline F3 safeNormalize(const F3& a, const F3& fallback) {
    const f32 l2 = dot(a, a);
    return l2 > 0.f ? a * (1.f / std::sqrt(l2)) : fallback;
}
/// Rows of a 3x4 matrix times (p, w).
FUSE_HOST_DEVICE inline F3 transform34(const f32* rows, const F3& p, f32 w) {
    return F3{rows[0] * p.x + rows[1] * p.y + rows[2] * p.z + rows[3] * w,
              rows[4] * p.x + rows[5] * p.y + rows[6] * p.z + rows[7] * w,
              rows[8] * p.x + rows[9] * p.y + rows[10] * p.z + rows[11] * w};
}
struct F4 {
    f32 x, y, z, w;
};
FUSE_HOST_DEVICE inline F4 operator+(const F4& a, const F4& b) { return F4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
FUSE_HOST_DEVICE inline F4 operator*(const F4& a, const F4& b) { return F4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
FUSE_HOST_DEVICE inline F4 operator*(const F4& a, f32 s) { return F4{a.x * s, a.y * s, a.z * s, a.w * s}; }
FUSE_HOST_DEVICE inline F4 lerp4(const F4& a, const F4& b, f32 t) {
    return F4{lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t), lerpf(a.w, b.w, t)};
}
/// Column-major 4x4 times (p, 1).
FUSE_HOST_DEVICE inline F4 transform44(const f32* m, const F3& p) {
    return F4{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
              m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14], m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15]};
}

// ---- packing (packing.slangh) -------------------------------------------------------------------------------------

/// f32ToUnorm8: floor(saturate(f) * 255 + 0.5) (spelled out: GLSL round() is implementation-defined at .5).
FUSE_HOST_DEVICE inline u32 f32ToUnorm8(f32 f) { return static_cast<u32>(std::floor(saturate(f) * 255.f + 0.5f)); }
FUSE_HOST_DEVICE inline u32 packUnorm4x8(const F4& c) {
    return f32ToUnorm8(c.x) | (f32ToUnorm8(c.y) << 8u) | (f32ToUnorm8(c.z) << 16u) | (f32ToUnorm8(c.w) << 24u);
}
FUSE_HOST_DEVICE inline F4 unpackUnorm4x8(u32 u) {
    return F4{static_cast<f32>(u & 0xFFu) * kInv255, static_cast<f32>((u >> 8u) & 0xFFu) * kInv255,
              static_cast<f32>((u >> 16u) & 0xFFu) * kInv255, static_cast<f32>(u >> 24u) * kInv255};
}

// ---- random numbers (noise.slangh) --------------------------------------------------------------------------------

/// "Prospector" 32-bit integer hash (upstream uintHash).
FUSE_HOST_DEVICE inline u32 uintHash(u32 x) {
    x ^= x >> 16u;
    x *= 0x21f0aaadu;
    x ^= x >> 15u;
    x *= 0x735a2d97u;
    x ^= x >> 15u;
    return x;
}
FUSE_HOST_DEVICE inline u32 uintHash3(u32 x, u32 y, u32 z) { return uintHash(x ^ uintHash(y) ^ uintHash(z)); }
/// [0, 1) from the low 23 bits (upstream unorm23ToFloat): exact on every backend.
FUSE_HOST_DEVICE inline f32 unorm23ToFloat(u32 x) { return asFloat((x & 0x007FFFFFu) | 0x3F800000u) - 1.f; }

/// The respawn random stream of one slot: (slot, frame, system seed) -> [0, 1).
struct SlotRandom {
    u32 seed;
    u32 frame;
    u32 system;
    FUSE_HOST_DEVICE f32 next() { return unorm23ToFloat(uintHash3(seed++, frame, system)); }
};

// ---- value noise and its curl (procedural_noise.slangh) -----------------------------------------------------------

inline constexpr i32 kValueNoiseResolution = 64;

/// One lattice texel of the 64^3 value-noise field: 4 unorm8 channels of a hash, as snorm.
FUSE_HOST_DEVICE inline F4 valueNoiseTexel(i32 x, i32 y, i32 z) {
    const u32 h = uintHash3(static_cast<u32>(x), static_cast<u32>(y) + 0x9E3779B9u, static_cast<u32>(z) + 0x7F4A7C15u);
    const F4 u = unpackUnorm4x8(h);
    return F4{u.x * 2.f - 1.f, u.y * 2.f - 1.f, u.z * 2.f - 1.f, u.w * 2.f - 1.f};
}
/// i mod 64 for any sign (two's complement mask; GLSL's `%` on negative ints is undefined, so both sides mask).
FUSE_HOST_DEVICE inline i32 wrapNoise(i32 i) {
    return static_cast<i32>(static_cast<u32>(i) & static_cast<u32>(kValueNoiseResolution - 1));
}
/// Trilinear sample at texel position `p` (texel centres at i + 0.5, repeat addressing), as a linear-filtered
/// sampler reads the upstream look-up texture.
FUSE_HOST_DEVICE inline F4 valueNoiseLutSample(const F3& p) {
    const F3 t = f3(p.x - 0.5f, p.y - 0.5f, p.z - 0.5f);
    const f32 fx = std::floor(t.x), fy = std::floor(t.y), fz = std::floor(t.z);
    const F3 f = f3(t.x - fx, t.y - fy, t.z - fz);
    const i32 x0 = wrapNoise(static_cast<i32>(fx)), y0 = wrapNoise(static_cast<i32>(fy)), z0 = wrapNoise(static_cast<i32>(fz));
    const i32 x1 = wrapNoise(x0 + 1), y1 = wrapNoise(y0 + 1), z1 = wrapNoise(z0 + 1);
    const F4 c00 = lerp4(valueNoiseTexel(x0, y0, z0), valueNoiseTexel(x1, y0, z0), f.x);
    const F4 c10 = lerp4(valueNoiseTexel(x0, y1, z0), valueNoiseTexel(x1, y1, z0), f.x);
    const F4 c01 = lerp4(valueNoiseTexel(x0, y0, z1), valueNoiseTexel(x1, y0, z1), f.x);
    const F4 c11 = lerp4(valueNoiseTexel(x0, y1, z1), valueNoiseTexel(x1, y1, z1), f.x);
    return lerp4(lerp4(c00, c10, f.y), lerp4(c01, c11, f.y), f.z);
}
FUSE_HOST_DEVICE inline f32 valueNoise4D(const F3& pos, f32 w) {
    const f32 angle = (w - std::floor(w)) * kTwoPi;
    const f32 ux = pcos(angle) * 0.5f + 0.5f;
    const f32 uy = psin(angle) * 0.5f + 0.5f;
    const F4 n = valueNoiseLutSample(pos);
    const f32 bottom = lerpf(n.x, n.y, ux);
    const f32 top = lerpf(n.z, n.w, ux);
    return lerpf(bottom, top, uy);
}
FUSE_HOST_DEVICE inline F3 noiseField(const F3& p, f32 w) {
    return F3{valueNoise4D(p + f3(37.1f, 17.2f, 19.3f), w + 101.1f), valueNoise4D(p + f3(29.9f, 11.4f, 5.7f), w + 45.6f),
              valueNoise4D(p + f3(11.0f, 59.5f, 47.8f), w + 13.2f)};
}
FUSE_HOST_DEVICE inline F3 curlOfValueNoise(const F3& pos, f32 time) {
    const f32 e = 0.01f;
    const F3 fpx = noiseField(pos + f3(e, 0.f, 0.f), time);
    const F3 fmx = noiseField(pos - f3(e, 0.f, 0.f), time);
    const F3 fpy = noiseField(pos + f3(0.f, e, 0.f), time);
    const F3 fmy = noiseField(pos - f3(0.f, e, 0.f), time);
    const F3 fpz = noiseField(pos + f3(0.f, 0.f, e), time);
    const F3 fmz = noiseField(pos - f3(0.f, 0.f, e), time);
    const f32 k = 0.5f / e;
    const F3 dFdx = (fpx - fmx) * k;
    const F3 dFdy = (fpy - fmy) * k;
    const F3 dFdz = (fpz - fmz) * k;
    return F3{dFdz.y - dFdy.z, dFdx.z - dFdz.x, dFdy.x - dFdx.y};
}

// ---- sampling (sampling.slangh, math.slangh) ----------------------------------------------------------------------

FUSE_HOST_DEVICE inline f32 signNotZero(f32 v) { return v >= 0.f ? 1.f : -1.f; }
FUSE_HOST_DEVICE inline void orthonormalBasis(const F3& n, F3& tangent, F3& bitangent) {
    const f32 sign = signNotZero(n.z);
    const f32 a = -1.f / (sign + n.z);
    const f32 b = n.x * n.y * a;
    tangent = F3{1.f + sign * n.x * n.x * a, sign * b, -sign * n.x};
    bitangent = F3{b, sign + n.y * n.y * a, -n.y};
}
/// A direction in the cone of half-angle acos(cosConeHalfAngle) around `axis` (unit).
FUSE_HOST_DEVICE inline F3 sampleDirectionInCone(const F3& axis, f32 cosConeHalfAngle, f32 u0, f32 u1) {
    const f32 phi = kTwoPi * u0;
    const f32 cosTheta = lerpf(cosConeHalfAngle, 1.f, u1);
    const f32 sinTheta = std::sqrt(maxf(0.f, 1.f - cosTheta * cosTheta));
    const F3 local = F3{sinTheta * pcos(phi), sinTheta * psin(phi), cosTheta};
    F3 t{}, b{};
    orthonormalBasis(axis, t, b);
    return t * local.x + b * local.y + axis * local.z;
}

// ---- the particle (GpuParticle methods) ---------------------------------------------------------------------------

FUSE_HOST_DEVICE inline bool isDead(const GpuParticle& p) { return p.state == kParticleDead; }
FUSE_HOST_DEVICE inline bool isSleeping(const GpuParticle& p) { return p.timeToLive <= 0.f; }
FUSE_HOST_DEVICE inline f32 initialTimeToLive(const GpuSystemDesc& d, f32 randSeed) {
    return maxf(minimumParticleLife(), lerpf(d.minTimeToLive, d.maxTimeToLive, randSeed));
}
/// 1 when born, 0 when dead.
FUSE_HOST_DEVICE inline f32 normalizedLife(const GpuSystemDesc& d, const GpuParticle& p) {
    return p.timeToLive / initialTimeToLive(d, p.randSeed);
}

// ---- kernel parameters --------------------------------------------------------------------------------------------

/// The pools every system shares (one set per frame); `constants` holds one entry per system and `system` names the
/// entry a launch runs (the GPU's push constant).
struct Params {
    kernel::Span<const GpuFrameConstants> constants;
    u32 system = 0;
    kernel::Span<GpuParticle> particles;
    kernel::Span<const GpuSpawnContext> spawnContexts;
    kernel::Span<const u32> spawnMap;
    kernel::Span<const f32> positions; ///< 3 per vertex
    kernel::Span<const u32> colors;    ///< RGBA8 per vertex
    kernel::Span<const f32> texcoords; ///< 2 per vertex
    kernel::Span<const u32> indices;
    kernel::Span<const Float4> animation;
    kernel::Span<GpuParticleVertex> vertices;
    kernel::Span<u32> counters;
};

// ---- animation table ----------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline F4 animationTexel(const Params& p, const GpuFrameConstants& c, u32 row, u32 x) {
    const Float4& t = p.animation[c.animationBase + row * kAnimationWidth + x];
    return F4{t.v[0], t.v[1], t.v[2], t.v[3]};
}
/// Bilinear read of row `row` at normalized life `u` (clamp to edge); with `randomize` the value is also blended
/// towards row + 1 by the particle's seed (upstream computeDataRow(row, true) on a linear sampler).
FUSE_HOST_DEVICE inline F4 sampleAnimation(const Params& p, const GpuFrameConstants& c, u32 row, f32 u, bool randomize,
                                           f32 seed) {
    const f32 tx = clampf(u * static_cast<f32>(kAnimationWidth) - 0.5f, 0.f, static_cast<f32>(kAnimationWidth - 1u));
    const f32 fx = std::floor(tx);
    const u32 x0 = static_cast<u32>(fx);
    const u32 x1 = x0 + 1u < kAnimationWidth ? x0 + 1u : kAnimationWidth - 1u;
    const f32 f = tx - fx;
    const F4 a = lerp4(animationTexel(p, c, row, x0), animationTexel(p, c, row, x1), f);
    if (!randomize) {
        return a;
    }
    const F4 b = lerp4(animationTexel(p, c, row + 1u, x0), animationTexel(p, c, row + 1u, x1), f);
    return lerp4(a, b, seed);
}
FUSE_HOST_DEVICE inline F4 particleColor(const Params& p, const GpuFrameConstants& c, const GpuParticle& q) {
    return unpackUnorm4x8(q.color) * sampleAnimation(p, c, kRowMinColor, normalizedLife(c.desc, q), true, q.randSeed);
}
FUSE_HOST_DEVICE inline void particleSize(const Params& p, const GpuFrameConstants& c, const GpuParticle& q, f32& sx, f32& sy) {
    const F4 s = sampleAnimation(p, c, kRowMinSize, normalizedLife(c.desc, q), true, q.randSeed);
    sx = s.x;
    sy = s.y;
}
FUSE_HOST_DEVICE inline f32 particleRotationSpeed(const Params& p, const GpuFrameConstants& c, const GpuParticle& q) {
    return sampleAnimation(p, c, kRowMinRotationSpeed, normalizedLife(c.desc, q), true, q.randSeed).x;
}
FUSE_HOST_DEVICE inline F3 particleMaxVelocity(const Params& p, const GpuFrameConstants& c, const GpuParticle& q) {
    const F4 v = sampleAnimation(p, c, kRowMaxVelocity, normalizedLife(c.desc, q), false, 0.f);
    return F3{v.x, v.y, v.z};
}

// ---- spawn ----------------------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline GpuParticle respawn(const Params& p, const GpuFrameConstants& c, u32 slot, const GpuSpawnContext& ctx) {
    const GpuSystemDesc& d = c.desc;
    SlotRandom rng{slot, c.frameIdx, c.systemSeed};
    const f32 r0 = rng.next();
    const f32 r1 = rng.next();
    const f32 r2 = rng.next();
    const f32 sum = r0 + r1 + r2;
    const F3 bary = sum > 0.f ? F3{saturate(r0 / sum), saturate(r1 / sum), saturate(r2 / sum)} : F3{1.f, 0.f, 0.f};
    u32 tri = static_cast<u32>(rng.next() * static_cast<f32>(ctx.triangleCount));
    tri = tri < ctx.triangleCount ? tri : ctx.triangleCount - 1u;

    F3 pos[3];
    F3 prev[3];
    const bool hasColors = (ctx.flags & kSpawnHasColors) != 0u;
    const bool hasUv = (ctx.flags & kSpawnHasTexcoords) != 0u;
    F4 color = hasColors ? F4{0.f, 0.f, 0.f, 0.f} : F4{1.f, 1.f, 1.f, 1.f};
    f32 uv[4] = {0.f, 0.f, 1.f, 1.f};
    const f32 baryW[3] = {bary.x, bary.y, bary.z};
    for (u32 i = 0; i < 3u; ++i) {
        const u32 index = p.indices[ctx.indexOffset + tri * 3u + i];
        const u32 v = ctx.vertexOffset + index;
        const u32 pv = ctx.prevVertexOffset + index;
        pos[i] = F3{p.positions[v * 3u], p.positions[v * 3u + 1u], p.positions[v * 3u + 2u]};
        prev[i] = F3{p.positions[pv * 3u], p.positions[pv * 3u + 1u], p.positions[pv * 3u + 2u]};
        if (hasColors) {
            color = color + unpackUnorm4x8(p.colors[v]) * baryW[i];
        }
        if (hasUv && (d.flags & kFlagUseSpawnTexcoords) != 0u) {
            const f32 tu = p.texcoords[v * 2u];
            const f32 tv = p.texcoords[v * 2u + 1u];
            if (i == 0u) {
                uv[0] = tu;
                uv[1] = tv;
                uv[2] = tu;
                uv[3] = tv;
            } else {
                uv[0] = minf(uv[0], tu);
                uv[1] = minf(uv[1], tv);
                uv[2] = maxf(uv[2], tu);
                uv[3] = maxf(uv[3], tv);
            }
        }
    }
    const f32 randSeed = rng.next();
    const F3 localPos = pos[0] * bary.x + pos[1] * bary.y + pos[2] * bary.z;
    const F3 localPrev = prev[0] * bary.x + prev[1] * bary.y + prev[2] * bary.z;
    const F3 prevWorld = transform34(ctx.prevObjectToWorld, localPrev, 1.f);
    const F3 world = transform34(ctx.objectToWorld, localPos, 1.f);
    const F3 spawnVelocity = (world - prevWorld) * c.invDeltaTimeSecs;

    const F3 localNormal = cross(safeNormalize(pos[1] - pos[0], f3(1.f, 0.f, 0.f)), safeNormalize(pos[2] - pos[0], f3(0.f, 1.f, 0.f)));
    const F3 worldNormal = safeNormalize(transform34(ctx.objectToWorld, localNormal, 0.f), f3(0.f, 0.f, 1.f));
    const f32 cosCone = pcos(d.initialVelocityConeAngleDegrees * kDegToRad);
    const f32 u0 = rng.next();
    const f32 u1 = rng.next();
    const F3 dir = sampleDirectionInCone(worldNormal, cosCone, u0, u1);
    const F3 velocity = spawnVelocity * d.initialVelocityFromMotion + dir * (d.initialVelocityFromNormal * c.sceneScale);
    const F3 spawnPos = F3{lerpf(world.x, prevWorld.x, randSeed), lerpf(world.y, prevWorld.y, randSeed),
                           lerpf(world.z, prevWorld.z, randSeed)};

    GpuParticle q{};
    store(q.position, spawnPos);
    q.color = packUnorm4x8(color);
    store(q.velocity, velocity);
    q.randSeed = randSeed;
    q.uvMinMax[0] = uv[0];
    q.uvMinMax[1] = uv[1];
    q.uvMinMax[2] = uv[2];
    q.uvMinMax[3] = uv[3];
    q.rotation = (randSeed * 2.f - 1.f) * (d.initialRotationDeviationDegrees * kDegToRad);
    q.timeToLive = initialTimeToLive(d, randSeed);
    q.state = kParticleAlive;
    q.pad = 0u;
    return q;
}

struct SpawnKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const GpuFrameConstants& c = p.constants[p.system];
        const u32 i = idx.linear;
        if (i >= c.spawnParticleCount) {
            return;
        }
        const u32 slot = (i + c.spawnParticleOffset) % c.desc.maxNumParticles;
        GpuParticle& q = p.particles[c.particleBase + slot];
        if (!isDead(q) && !isSleeping(q)) {
            return;
        }
        const GpuSpawnContext& ctx = p.spawnContexts[p.spawnMap[c.spawnMapBase + i]];
        if (ctx.triangleCount == 0u) {
            return;
        }
        q = respawn(p, c, slot, ctx);
    }
};

// ---- evolve ---------------------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline F3 attractorAcceleration(const GpuFrameConstants& c, const F3& pos) {
    const GpuSystemDesc& d = c.desc;
    const F3 toAttractor = f3(d.attractorPosition) - pos;
    const f32 distSq = maxf(dot(toAttractor, toAttractor), 1e-4f);
    const f32 scaledRadius = d.attractorRadius * c.sceneScale;
    const f32 invRadius = 1.f / maxf(scaledRadius, 1e-3f);
    const f32 falloff = 1.f / (1.f + distSq * invRadius * invRadius);
    return toAttractor * ((1.f / std::sqrt(distSq)) * (d.attractorForce * c.sceneScale * falloff));
}

FUSE_HOST_DEVICE inline void evolve(const Params& p, const GpuFrameConstants& c, GpuParticle& q) {
    const GpuSystemDesc& d = c.desc;
    const f32 dt = c.deltaTimeSecs;
    F3 v = f3(q.velocity);
    F3 pos = f3(q.position);
    if ((d.flags & kFlagUseTurbulence) != 0u) {
        const f32 freq = d.turbulenceFrequency * c.sceneScale;
        v = v + curlOfValueNoise(pos * freq, c.absoluteTimeSecs * freq) * (d.turbulenceForce * c.sceneScale * dt);
    }
    v = v + f3(c.upDirection) * (d.gravityForce * c.sceneScale * dt);
    v = v + attractorAcceleration(c, pos) * dt;
    v = v * maxf(1.f - d.dragCoefficient * dt, 0.f);
    if ((d.flags & kFlagRestrictVelocityX) != 0u) {
        v.x = 0.f;
    }
    if ((d.flags & kFlagRestrictVelocityY) != 0u) {
        v.y = 0.f;
    }
    if ((d.flags & kFlagRestrictVelocityZ) != 0u) {
        v.z = 0.f;
    }
    const F3 maxV = particleMaxVelocity(p, c, q) * c.sceneScale;
    v.x = maxV.x > 0.f ? clampf(v.x, -maxV.x, maxV.x) : v.x;
    v.y = maxV.y > 0.f ? clampf(v.y, -maxV.y, maxV.y) : v.y;
    v.z = maxV.z > 0.f ? clampf(v.z, -maxV.z, maxV.z) : v.z;
    pos = pos + v * dt;
    store(q.velocity, v);
    store(q.position, pos);
    if ((d.flags & kFlagEnableMotionTrail) == 0u) {
        if ((d.flags & kFlagAlignParticlesToVelocity) != 0u) {
            q.rotation = 0.f;
        } else {
            const f32 r = q.rotation + particleRotationSpeed(p, c, q) * dt;
            q.rotation = r - kTwoPi * std::trunc(r / kTwoPi);
        }
    }
}

struct EvolveKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const GpuFrameConstants& c = p.constants[p.system];
        const u32 i = idx.linear;
        if (i >= c.simulateParticleCount) {
            return;
        }
        const u32 slot = (i + c.particleTailOffset) % c.desc.maxNumParticles;
        GpuParticle q = p.particles[c.particleBase + slot];
        if (isDead(q)) {
            return;
        }
        if (!isSleeping(q)) {
            evolve(p, c, q);
        }
        q.timeToLive = q.timeToLive - c.deltaTimeSecs;
        // Conservative counting: a particle is retired (and counted) only at the death time of the longest life,
        // so retirements happen in spawn (ring) order.
        const f32 retireAt = initialTimeToLive(c.desc, q.randSeed) - c.desc.maxTimeToLive;
        if (q.timeToLive <= retireAt) {
            q.state = kParticleDead;
            kernel::global_atomic_add(&p.counters[c.counterIndex], 1u);
        }
        p.particles[c.particleBase + slot] = q;
    }
};

// ---- billboards -----------------------------------------------------------------------------------------------------

FUSE_HOST_DEVICE inline void clearVertices(const Params& p, const GpuFrameConstants& c, u32 i) {
    for (u32 k = 0; k < c.verticesPerParticle; ++k) {
        p.vertices[c.vertexBase + i * c.verticesPerParticle + k] = GpuParticleVertex{};
    }
}
/// Upstream calcBillboardSinCos: the normalized screen-space velocity (sin, cos).
FUSE_HOST_DEVICE inline void billboardSinCos(const F3& velocity, const F3& right, const F3& up, f32& s, f32& co) {
    const f32 len = length(velocity);
    if (len < 1e-6f) {
        s = 0.f;
        co = 1.f;
        return;
    }
    const F3 v = velocity * (1.f / len);
    const f32 x = -dot(v, right);
    const f32 y = dot(v, up);
    const f32 l = std::sqrt(x * x + y * y);
    if (l > 0.f) {
        s = x / l;
        co = y / l;
    } else {
        s = 0.f;
        co = 1.f;
    }
}
FUSE_HOST_DEVICE inline F3 projectOntoPlaneDir(const F3& v, const F3& n, const F3& fallback) {
    const F3 pr = v - n * dot(v, n);
    const f32 l2 = dot(pr, pr);
    return l2 > 1e-12f ? pr * (1.f / std::sqrt(l2)) : fallback;
}
FUSE_HOST_DEVICE inline void randomFlip(u32 mode, f32 seed, f32& fx, f32& fy) {
    fx = 1.f;
    fy = 1.f;
    if (mode == static_cast<u32>(RandomFlipAxis::Horizontal)) {
        fx = seed < 0.5f ? -1.f : 1.f;
    } else if (mode == static_cast<u32>(RandomFlipAxis::Vertical)) {
        fy = seed < 0.5f ? -1.f : 1.f;
    } else if (mode == static_cast<u32>(RandomFlipAxis::Both)) {
        if (seed < 0.25f) {
            fx = -1.f;
            fy = -1.f;
        } else if (seed < 0.5f) {
            fy = -1.f;
        } else if (seed < 0.75f) {
            fx = -1.f;
        }
    }
}
/// Upstream chopMantissaLSB(value, 4): clears the 4 low mantissa bits.
FUSE_HOST_DEVICE inline f32 chopMantissa(f32 v) { return asFloat(asUint(v) & 0xFFFFFFF0u); }

/// The billboard basis (right, up) of a particle at `pos`.
FUSE_HOST_DEVICE inline void billboardBasis(const GpuFrameConstants& c, const F3& pos, F3& right, F3& up) {
    const F3 camRight = F3{c.viewToWorld[0], c.viewToWorld[1], c.viewToWorld[2]};
    const F3 camUp = F3{c.viewToWorld[4], c.viewToWorld[5], c.viewToWorld[6]};
    const F3 camPos = F3{c.viewToWorld[12], c.viewToWorld[13], c.viewToWorld[14]};
    const F3 worldUp = safeNormalize(f3(c.upDirection), f3(0.f, 1.f, 0.f));
    const u32 type = c.desc.billboardType;
    if (type == static_cast<u32>(BillboardType::FaceCameraUpAxisLocked)) {
        const F3 toCam = safeNormalize(camPos - pos, f3(0.f, 0.f, 1.f));
        const F3 plane = projectOntoPlaneDir(toCam, worldUp, f3(0.f, 0.f, 1.f));
        right = safeNormalize(cross(plane, worldUp), f3(1.f, 0.f, 0.f));
        up = worldUp;
    } else if (type == static_cast<u32>(BillboardType::FaceCameraPosition)) {
        const F3 n = safeNormalize(pos - camPos, f3(0.f, 0.f, 1.f));
        const F3 upInPlane = projectOntoPlaneDir(worldUp, n, f3(0.f, 0.f, 1.f));
        right = safeNormalize(cross(upInPlane, n), f3(1.f, 0.f, 0.f));
        up = upInPlane;
    } else if (type == static_cast<u32>(BillboardType::FaceWorldUp)) {
        const F3 ref = std::fabs(worldUp.z) < 0.98f ? f3(0.f, 0.f, 1.f) : f3(1.f, 0.f, 0.f);
        const F3 xAxis = projectOntoPlaneDir(ref, worldUp, f3(1.f, 0.f, 0.f));
        right = xAxis;
        up = safeNormalize(cross(worldUp, xAxis), f3(0.f, 0.f, 1.f));
    } else {
        right = camRight;
        up = camUp;
    }
}

struct BillboardKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const GpuFrameConstants& c = p.constants[p.system];
        const GpuSystemDesc& d = c.desc;
        const u32 i = idx.linear;
        if (i >= d.maxNumParticles) {
            return;
        }
        if (i >= c.particleCount) {
            clearVertices(p, c, i);
            return;
        }
        const u32 slot = (i + c.particleTailOffset) % d.maxNumParticles;
        const GpuParticle q = p.particles[c.particleBase + slot];
        if (isDead(q) || isSleeping(q)) {
            clearVertices(p, c, i);
            return;
        }
        f32 sx = 0.f, sy = 0.f;
        particleSize(p, c, q, sx, sy);
        sx *= c.sceneScale;
        sy *= c.sceneScale;
        const F4 color = particleColor(p, c, q);
        if (color.w < c.resolveTransparencyThreshold || maxf(sx, sy) < 0.1f * c.sceneScale) {
            clearVertices(p, c, i);
            return;
        }
        const F3 pos = f3(q.position);
        F3 right{}, up{};
        billboardBasis(c, pos, right, up);

        // Screen-space size culling against the previous frame's projection.
        {
            const F4 center = transform44(c.prevWorldToProjection, pos);
            const F4 offset = transform44(c.prevWorldToProjection, pos + right * sx + up * sy);
            const f32 dx = (offset.x / offset.w - center.x / center.w) * static_cast<f32>(c.renderingWidth);
            const f32 dy = (offset.y / offset.w - center.y / center.w) * static_cast<f32>(c.renderingHeight);
            if (dx * dx + dy * dy <= c.minParticleSize * c.minParticleSize) {
                clearVertices(p, c, i);
                return;
            }
        }

        const F3 vel = f3(q.velocity);
        f32 rs = 0.f, rc = 1.f;
        if ((d.flags & kFlagAlignParticlesToVelocity) != 0u) {
            billboardSinCos(vel, right, up, rs, rc);
        } else {
            rs = psin(q.rotation);
            rc = pcos(q.rotation);
        }

        const u32 rows = d.spriteSheetRows > 1u ? d.spriteSheetRows : 1u;
        const u32 cols = d.spriteSheetCols > 1u ? d.spriteSheetCols : 1u;
        const u32 frames = rows * cols;
        u32 frame = 0u;
        if (frames > 1u) {
            if (d.spriteSheetMode == static_cast<u32>(SpriteSheetMode::OverrideMaterialLifetime)) {
                const f32 lifeFrac = 1.f - normalizedLife(d, q);
                const f32 fi = std::floor(lifeFrac * static_cast<f32>(frames));
                frame = fi < 0.f ? 0u : static_cast<u32>(fi);
            } else if (d.spriteSheetMode == static_cast<u32>(SpriteSheetMode::OverrideMaterialRandom)) {
                frame = static_cast<u32>(q.randSeed * static_cast<f32>(frames));
            }
            frame = frame < frames - 1u ? frame : frames - 1u;
        }
        const f32 cellW = (q.uvMinMax[2] - q.uvMinMax[0]) / static_cast<f32>(cols);
        const f32 cellH = (q.uvMinMax[3] - q.uvMinMax[1]) / static_cast<f32>(rows);
        const f32 frameU = q.uvMinMax[0] + static_cast<f32>(frame % cols) * cellW;
        const f32 frameV = q.uvMinMax[1] + static_cast<f32>(frame / cols) * cellH;
        f32 flipX = 1.f, flipY = 1.f;
        randomFlip(d.randomFlipAxis, q.randSeed, flipX, flipY);
        const u32 colorEnc = packUnorm4x8(F4{color.z, color.y, color.x, color.w});
        const bool trail = (d.flags & kFlagEnableMotionTrail) != 0u;

        for (u32 k = 0; k < c.verticesPerParticle; ++k) {
            const f32 ox = c.vertexOffsets[k * 2u];
            const f32 oy = c.vertexOffsets[k * 2u + 1u];
            F3 worldOffset{};
            if (trail) {
                const f32 mx = -dot(vel, right);
                const f32 my = -dot(vel, up);
                const f32 speed = std::sqrt(mx * mx + my * my);
                const f32 dirX = speed > 0.f ? mx / speed : 0.f;
                const f32 dirY = speed > 0.f ? my / speed : 1.f;
                const f32 orthoX = -dirY;
                const f32 orthoY = dirX;
                const f32 width = ox * sx;
                const f32 len = k >= 4u ? speed * d.motionTrailMultiplier * c.deltaTimeSecs : 0.f;
                const f32 height = oy * sy + len;
                const f32 px = width * orthoX + height * dirX;
                const f32 py = width * orthoY + height * dirY;
                worldOffset = right * px + up * py;
            } else {
                const f32 lx = ox * sx;
                const f32 ly = oy * sy;
                const f32 rx = rc * lx - rs * ly;
                const f32 ry = rs * lx + rc * ly;
                worldOffset = right * rx + up * ry;
            }
            const F3 vp = pos + worldOffset;
            GpuParticleVertex out{};
            out.position[0] = chopMantissa(vp.x);
            out.position[1] = chopMantissa(vp.y);
            out.position[2] = chopMantissa(vp.z);
            out.color = colorEnc;
            const f32 uvx = ox;
            const f32 uvy = trail ? oy : -oy;
            out.texcoord[0] = (uvx * flipX + 0.5f) * cellW + frameU;
            out.texcoord[1] = (uvy * flipY + 0.5f) * cellH + frameV;
            p.vertices[c.vertexBase + i * c.verticesPerParticle + k] = out;
        }
    }
};

/// Quad corners (upstream ParticleSystem constructor): 4 for a classic billboard, 8 (tail then head) with a motion
/// trail.
inline constexpr f32 kQuadOffsets[8] = {-0.5f, 0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f};
inline constexpr f32 kTrailOffsets[16] = {-0.5f, -0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.5f, 0.0f,
                                          -0.5f, 0.0f,  -0.5f, 0.5f, 0.5f, 0.0f,  0.5f, 0.5f};

} // namespace fuse::relight::particles::kernels
