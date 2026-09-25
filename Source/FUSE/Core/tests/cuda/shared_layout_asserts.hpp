#pragma once

// B1 row "CUDA compiles against C++23 FUSE host headers": one set of layout assertions over the
// *shared* FUSE types, included by the C++23 host TU (g++ -std=c++23) and by the .cu TU, where nvcc
// evaluates it twice — host pass (C++20, g++-12) and device pass (__CUDA_ARCH__). Any drift in
// size/alignment/offsets between the host C++23 build and CUDA device code fails to compile.

#include <fuse/gria.hpp>
#include <fuse/math/math.hpp>
#include <fuse/types.hpp>

#include <cstddef>
#include <type_traits>

namespace fuse::cuda_gate {

// Scalar vocabulary (fuse/types.hpp) — identical widths on host and device.
static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);
static_assert(sizeof(s32) == 4 && sizeof(s64) == 8 && sizeof(f32) == 4 && sizeof(f64) == 8);
static_assert(sizeof(usize) == 8, "FUSE CUDA lane targets 64-bit hosts and devices");

// Math types (fuse/math/*) — plain f32 aggregates, memcpy-able across the host/device boundary.
using math::Mat3;
using math::Mat4;
using math::Quat;
using math::Vec2;
using math::Vec3;
using math::Vec4;

static_assert(sizeof(Vec2) == 8 && alignof(Vec2) == 4);
static_assert(sizeof(Vec3) == 12 && alignof(Vec3) == 4);
static_assert(sizeof(Vec4) == 16 && alignof(Vec4) == 4);
static_assert(sizeof(Quat) == 16 && alignof(Quat) == 4);
static_assert(sizeof(Mat3) == 36 && alignof(Mat3) == 4);
static_assert(sizeof(Mat4) == 64 && alignof(Mat4) == 4);
static_assert(offsetof(Vec3, x) == 0 && offsetof(Vec3, y) == 4 && offsetof(Vec3, z) == 8);
static_assert(offsetof(Vec4, w) == 12);
static_assert(offsetof(Quat, x) == 0 && offsetof(Quat, w) == 12);
static_assert(offsetof(Mat4, data) == 0);

static_assert(std::is_trivially_copyable_v<Vec2> && std::is_standard_layout_v<Vec2>);
static_assert(std::is_trivially_copyable_v<Vec3> && std::is_standard_layout_v<Vec3>);
static_assert(std::is_trivially_copyable_v<Vec4> && std::is_standard_layout_v<Vec4>);
static_assert(std::is_trivially_copyable_v<Quat> && std::is_standard_layout_v<Quat>);
static_assert(std::is_trivially_copyable_v<Mat3> && std::is_standard_layout_v<Mat3>);
static_assert(std::is_trivially_copyable_v<Mat4> && std::is_standard_layout_v<Mat4>);

// GRIA (fuse/gria.hpp) — constexpr, evaluated identically by every compiler pass.
static_assert(sizeof(Alpha) == 4 && alignof(Alpha) == 4);
static_assert(std::is_trivially_copyable_v<Alpha> && std::is_standard_layout_v<Alpha>);
static_assert(Alpha(2.f).get() == 1.f && Alpha(-1.f).get() == 0.f);
static_assert(ALPHA_CHAOS_EDGE.at_edge_of_chaos() && ALPHA_EXACT.is_exact() && ALPHA_APPROXIMATE.is_approximate());
static_assert(Alpha::from_entropy_ratio(1.f, 4.f).get() == 0.75f);
static_assert(Alpha(0.25f).blend(2.f, 6.f) == 3.f);

/// Fixed inputs shared by the host-parity check and the device kernel.
struct GateSample {
    Vec3 point;
    Quat rotation;
    Vec3 translation;
    Vec3 scale;
    f32 alpha;
};
static_assert(std::is_trivially_copyable_v<GateSample>);
static_assert(sizeof(GateSample) == 12 + 16 + 12 + 12 + 4);

/// Per-sample result: 3 floats of transformed point, 1 SDF value, 4 floats of slerped quaternion.
inline constexpr u32 kGateResultFloats = 8;

/// The single definition of the gate computation. Compiled by g++ (C++23 host), by nvcc's host pass,
/// and by nvcc's device pass (inside a __global__ kernel) — no CUDA-specific copy of the math.
FUSE_HOST_DEVICE inline void evaluateGateSample(const GateSample& s, f32* out) {
    const Mat4 trs = math::fromTRS(s.translation, s.rotation.normalized(), s.scale);
    const Mat4 composed = math::multiply(trs, Mat4::identity());
    const Vec3 world = math::transformPoint(composed, s.point);
    const Vec3 rotated = s.rotation.normalized().rotate(math::cross(world, Vec3{0.f, 1.f, 0.f}) + world);
    const Alpha a(s.alpha);
    const f32 sdf = math::SDF::opSmoothUnion(math::SDF::sphere(rotated, 1.f),
                                             math::SDF::box(rotated, Vec3{0.5f, 0.5f, 0.5f}), a.blend(0.05f, 0.5f));
    const Quat q = math::slerp(Quat::identity(), s.rotation, a.get());
    out[0] = rotated.x;
    out[1] = rotated.y;
    out[2] = rotated.z;
    out[3] = sdf;
    out[4] = q.x;
    out[5] = q.y;
    out[6] = q.z;
    out[7] = q.w;
}

inline constexpr u32 kGateSampleCount = 4;

inline GateSample gateSample(u32 i) {
    const f32 t = static_cast<f32>(i);
    GateSample s{};
    s.point = Vec3{0.25f + t, -0.5f * t, 1.f};
    s.rotation = math::fromAxisAngle(Vec3{0.f, 1.f, 1.f}, 0.3f + 0.2f * t);
    s.translation = Vec3{1.f, 2.f, 3.f};
    s.scale = Vec3{1.f, 1.f + 0.5f * t, 1.f};
    s.alpha = 0.2f * t;
    return s;
}

} // namespace fuse::cuda_gate
