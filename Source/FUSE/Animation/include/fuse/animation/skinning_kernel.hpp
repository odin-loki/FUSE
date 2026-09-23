#pragma once

// Single-source linear blend skinning (docs/compute-kernels.md, "Skinning": item kernel, one vertex per
// item, bone palette in a Span<const mat4>). This body is the ONLY skinning implementation: the CPU
// backends (src/skinning.cpp) and the CUDA trampoline (kernels/skinning.cu) both run it.
//
// Device-safe: only POD field access, std::sqrt and constexpr std::array accessors (nvcc
// --expt-relaxed-constexpr). The CPU path has no dual-quaternion mode, so neither does the kernel.

#include <fuse/animation/skinning.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::animation::skinning_kernel {

/// Kernel / profiler / GPU-timestamp name.
inline constexpr const char* kName = "skinning_lbs";
/// One vertex per thread; 128 fits Vulkan's guaranteed workgroup size and a CUDA warp multiple.
inline constexpr kernel::Dim3 kWorkgroup{128u, 1u, 1u};

struct Params {
    kernel::Span<const vec3> rest_positions;
    kernel::Span<const vec3> rest_normals; ///< Empty: no normals are produced.
    kernel::Span<const SkinningWeights> weights;
    kernel::Span<const mat4> bones;
    kernel::Span<vec3> out_positions;
    kernel::Span<vec3> out_normals; ///< Same size as rest_normals.
};

/// Skins vertex `v`: sum over up to 4 influences (weight > 0, bone index in range) of the bone
/// transform applied to the rest position / rest normal (linear part), normal renormalized.
FUSE_HOST_DEVICE inline void skin_vertex(const Params& p, u32 v) {
    const bool hasNormals = !p.rest_normals.empty();
    const vec3 restPos = p.rest_positions[v];
    vec3 restNormal{};
    if (hasNormals) {
        restNormal = p.rest_normals[v];
    }
    const SkinningWeights w = p.weights[v];

    f32 px = 0.f, py = 0.f, pz = 0.f;
    f32 nx = 0.f, ny = 0.f, nz = 0.f;
    for (u32 influence = 0; influence < 4u; ++influence) {
        const f32 weight = w.bone_weights[influence];
        if (weight <= 0.f) {
            continue;
        }
        const u32 boneIndex = w.bone_indices[influence];
        if (boneIndex >= p.bones.size) {
            continue;
        }
        const f32* m = p.bones[boneIndex].data.data();
        const f32 tx = m[0] * restPos.x + m[4] * restPos.y + m[8] * restPos.z + m[12];
        const f32 ty = m[1] * restPos.x + m[5] * restPos.y + m[9] * restPos.z + m[13];
        const f32 tz = m[2] * restPos.x + m[6] * restPos.y + m[10] * restPos.z + m[14];
        const f32 ux = m[0] * restNormal.x + m[4] * restNormal.y + m[8] * restNormal.z;
        const f32 uy = m[1] * restNormal.x + m[5] * restNormal.y + m[9] * restNormal.z;
        const f32 uz = m[2] * restNormal.x + m[6] * restNormal.y + m[10] * restNormal.z;
        px += tx * weight;
        py += ty * weight;
        pz += tz * weight;
        nx += ux * weight;
        ny += uy * weight;
        nz += uz * weight;
    }

    vec3 outPos{};
    outPos.x = px;
    outPos.y = py;
    outPos.z = pz;
    p.out_positions[v] = outPos;
    if (hasNormals) {
        const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) {
            nx = nx / len;
            ny = ny / len;
            nz = nz / len;
        }
        vec3 outNormal{};
        outNormal.x = nx;
        outNormal.y = ny;
        outNormal.z = nz;
        p.out_normals[v] = outNormal;
    }
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        skin_vertex(p, idx.linear);
    }
};

/// Validation shared by every backend (mirrors skin_vertices' contract).
inline bool params_valid(const Params& p) {
    const u32 n = p.rest_positions.size;
    return n > 0u && p.weights.size == n && p.out_positions.size == n &&
           (p.rest_normals.empty() || p.rest_normals.size == n) && p.out_normals.size == p.rest_normals.size &&
           p.rest_positions.data != nullptr && p.weights.data != nullptr && p.out_positions.data != nullptr;
}

inline kernel::KernelLaunch make_launch(const Params& p) {
    return kernel::KernelLaunch{kName, kernel::extent1(p.rest_positions.size), kWorkgroup};
}

/// Params over the host vectors of `input` / `output` (output must already be sized).
inline Params make_params(const SkinningInput& input, SkinningOutput& output) {
    Params p{};
    p.rest_positions = {input.rest_positions.data(), static_cast<u32>(input.rest_positions.size())};
    p.rest_normals = {input.rest_normals.data(), static_cast<u32>(input.rest_normals.size())};
    p.weights = {input.weights.data(), static_cast<u32>(input.weights.size())};
    p.bones = {input.bone_transforms.data(), static_cast<u32>(input.bone_transforms.size())};
    p.out_positions = {output.positions.data(), static_cast<u32>(output.positions.size())};
    p.out_normals = {output.normals.data(), static_cast<u32>(output.normals.size())};
    return p;
}

} // namespace fuse::animation::skinning_kernel
