#pragma once

// RL-5.5 radiance denoiser: the C++ dialect of the single-source core (shaders/denoise/rdn_core.h, which documents the
// algorithm, the passes and the RdnFrame record). It supplies the HLSL-style vector types, the float math overloads
// the core calls unqualified, the dialect macros and the buffer accessors, then includes the core inside namespace
// fuse::renderer::denoise::rdnk. The accessors read / write host memory: the CPU reference (RdnReference) stores host
// pointers in the RdnFrame address fields, the GPU (RadianceDenoiser) device addresses. Everything is inline and
// allocation-free, so the passes run as compute_kernel bodies on the CPU backends (CpuReference == CpuParallel bit
// for bit); against the GPU (Slang / GLSL twins of the same text) the differences are the device's exp / sqrt /
// division rounding and multiply-add contraction (fuse_rp_rdn_vk_passes documents the tolerance).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace fuse::renderer::denoise::rdnk {

using uint = std::uint32_t;
using std::uint64_t;

struct float2 {
    float x = 0.f;
    float y = 0.f;
    constexpr float2() = default;
    constexpr float2(float a, float b) : x(a), y(b) {}
};

struct float3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    constexpr float3() = default;
    constexpr float3(float a, float b, float c) : x(a), y(b), z(c) {}
};

struct float4 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float w = 0.f;
    constexpr float4() = default;
    constexpr float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
static_assert(sizeof(float2) == 8 && sizeof(float4) == 16, "rdn records");

constexpr float3 operator+(const float3& a, const float3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr float3 operator-(const float3& a, const float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr float3 operator*(const float3& a, const float3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
constexpr float3 operator*(const float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
constexpr float4 operator+(const float4& a, const float4& b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
constexpr float4 operator*(const float4& a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }

// Scalar math: float overloads only (no double promotion in the core).
inline float sqrt(float x) { return std::sqrt(x); }
inline float exp(float x) { return std::exp(x); }
inline float floor(float x) { return std::floor(x); }
inline float abs(float x) { return std::fabs(x); }
constexpr float min(float a, float b) { return b < a ? b : a; }
constexpr float max(float a, float b) { return a < b ? b : a; }
constexpr float dot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 normalize(const float3& a) {
    const float l = sqrt(dot(a, a));
    return float3(a.x / l, a.y / l, a.z / l);
}

// Buffer accessors over host memory (the address fields hold host pointers on the CPU).
inline float4 rdnLd4(uint64_t a, uint i) { return reinterpret_cast<const float4*>(static_cast<std::uintptr_t>(a))[i]; }
inline void rdnSt4(uint64_t a, uint i, float4 v) { reinterpret_cast<float4*>(static_cast<std::uintptr_t>(a))[i] = v; }
inline float2 rdnLd2(uint64_t a, uint i) { return reinterpret_cast<const float2*>(static_cast<std::uintptr_t>(a))[i]; }
inline float rdnLd1(uint64_t a, uint i) { return reinterpret_cast<const float*>(static_cast<std::uintptr_t>(a))[i]; }
inline uint rdnLdU(uint64_t a, uint i) { return reinterpret_cast<const uint*>(static_cast<std::uintptr_t>(a))[i]; }

#define FUSE_RDN_FN inline
#define FUSE_RDN_IN(T) const T&
#define FUSE_RDN_INOUT(T) T&

#include "rdn_core.h"

#undef FUSE_RDN_FN
#undef FUSE_RDN_IN
#undef FUSE_RDN_INOUT

/// Host pointer -> address field.
inline uint64_t rdnAddr(const void* p) { return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(p)); }

/// The push arguments of one pass (the GPU's RdnPush minus the frame address).
struct RdnPassArgs {
    uint pass = 0;
    uint step = 1;
    uint64_t a[6] = {0, 0, 0, 0, 0, 0};
};

struct RdnKernelParams {
    RdnFrame frame{};
    RdnPassArgs args{};
};

/// compute_kernel body of every pass (one item per pixel / stratum / pyramid texel).
struct RdnKernel {
    void operator()(const kernel::LaunchIndex& idx, const RdnKernelParams& p) const {
        rdnRun(p.frame, p.args.pass, p.args.step, p.args.a[0], p.args.a[1], p.args.a[2], p.args.a[3], p.args.a[4],
               p.args.a[5], idx.global.x, idx.global.y);
    }
};

} // namespace fuse::renderer::denoise::rdnk
