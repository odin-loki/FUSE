#pragma once
// Shared by the GPU skinning gates (test_rp_skinning.cpp) and the skinned motion-vector case of test_rp_temporal.cpp:
// rest streams of a GPU-scene meshlet mesh, a procedural bone chain along the mesh's y extent, its LBS palette per
// time step (bend about x / z around each joint, column-major world * inverse bind) and f64 LBS of one vertex.

#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/geometry/meshlet_types.hpp>
#include <fuse/renderer/geometry/vertex_codec_kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/skinning/skinning_gpu_types.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>

#include <cmath>
#include <vector>

namespace sk_test {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::renderer::skinning::SkinVertexWeights;

/// Rest pose of a meshlet mesh exactly as the static row decodes it (dequantised VPOS, decoded VNRM, handedness), and
/// a `bones`-bone chain along y: each vertex blends its two nearest joints (smoothstep), every 7th vertex gets a
/// third influence, every 13th an out-of-range bone (skipped by the kernel) and a zero weight.
struct SkinMesh {
    std::vector<f32> positions; ///< xyz
    std::vector<f32> normals;   ///< xyz
    std::vector<f32> handedness;
    std::vector<SkinVertexWeights> weights;
    u32 vertexCount = 0;
    u32 bones = 0;
    f32 y0 = 0.f; ///< chain start (lowest vertex y)
    f32 y1 = 1.f; ///< chain end
    f32 cx = 0.f; ///< chain axis (x, z) = bounds centre
    f32 cz = 0.f;
};

inline SkinMesh makeSkinMesh(const fuse::renderer::geometry::MeshletMesh& mesh, const fuse::renderer::gpu_scene::GpuMesh& row,
                             u32 bones) {
    SkinMesh s;
    s.vertexCount = mesh.vertex_count();
    s.bones = bones;
    s.positions.resize(s.vertexCount * 3u);
    s.normals.resize(s.vertexCount * 3u);
    s.handedness.resize(s.vertexCount);
    s.weights.resize(s.vertexCount);
    f32 lo = 1e30f;
    f32 hi = -1e30f;
    for (u32 v = 0; v < s.vertexCount; ++v) {
        fuse::renderer::visbuffer::decode_kernel::mesh_position(row, mesh.positions.data(), v, &s.positions[v * 3u]);
        fuse::renderer::geometry::vertex_codec::oct_decode(mesh.normals[v], &s.normals[v * 3u]);
        s.handedness[v] = (mesh.positions[v * 4u + 3u] & fuse::renderer::geometry::kVposTangentNegative) != 0u ? -1.f : 1.f;
        lo = std::fmin(lo, s.positions[v * 3u + 1u]);
        hi = std::fmax(hi, s.positions[v * 3u + 1u]);
    }
    s.y0 = lo;
    s.y1 = hi > lo ? hi : lo + 1.f;
    s.cx = row.boundsCenter[0];
    s.cz = row.boundsCenter[2];
    const f32 seg = (s.y1 - s.y0) / static_cast<f32>(bones);
    for (u32 v = 0; v < s.vertexCount; ++v) {
        const f32 t = (s.positions[v * 3u + 1u] - s.y0) / seg - 0.5f; // joint b sits at y0 + (b + 0.5) seg
        const f32 ft = std::floor(t);
        const u32 b0 = t <= 0.f ? 0u : (static_cast<u32>(ft) >= bones - 1u ? bones - 1u : static_cast<u32>(ft));
        const u32 b1 = b0 + 1u < bones ? b0 + 1u : b0;
        f32 a = t - static_cast<f32>(b0);
        a = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
        a = a * a * (3.f - 2.f * a);
        SkinVertexWeights w{};
        w.bone[0] = b0;
        w.bone[1] = b1;
        w.weight[0] = 1.f - a;
        w.weight[1] = a;
        if (v % 7u == 3u && bones > 2u) {
            w.bone[2] = (b0 + 2u) % bones;
            w.weight[2] = 0.15f;
            w.weight[0] *= 0.85f;
            w.weight[1] *= 0.85f;
        }
        if (v % 13u == 5u) {
            w.bone[3] = bones + 4u; // out of range: skipped
            w.weight[3] = 0.4f;
            w.bone[2] = 0u;
            w.weight[2] = 0.f; // zero weight: skipped
        }
        s.weights[v] = w;
    }
    return s;
}

/// Column-major 4x4 helpers in f64.
struct M4 {
    f64 m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
inline M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (u32 c = 0; c < 4u; ++c) {
        for (u32 rr = 0; rr < 4u; ++rr) {
            f64 s = 0.0;
            for (u32 k = 0; k < 4u; ++k) {
                s += a.m[k * 4u + rr] * b.m[c * 4u + k];
            }
            r.m[c * 4u + rr] = s;
        }
    }
    return r;
}
inline M4 translate(f64 x, f64 y, f64 z) {
    M4 r;
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}
inline M4 rotXZ(f64 ax, f64 az) {
    const f64 cx = std::cos(ax), sx = std::sin(ax), cz = std::cos(az), sz = std::sin(az);
    M4 rx;
    rx.m[5] = cx;
    rx.m[6] = sx;
    rx.m[9] = -sx;
    rx.m[10] = cx;
    M4 rz;
    rz.m[0] = cz;
    rz.m[1] = sz;
    rz.m[4] = -sz;
    rz.m[5] = cz;
    return mul(rz, rx);
}

/// Palette of the chain at time `t` (phase per character): bone b = parent * T(joint) R(angles) T(-joint), plus a small
/// root sway; written as f32 column-major (what the GPU and the CPU kernel both consume).
inline void palette(const SkinMesh& s, f32 t, f32 phase, f32 amplitude, f32* out) {
    const f64 seg = (static_cast<f64>(s.y1) - s.y0) / s.bones;
    M4 parent = translate(0.03 * std::sin(1.3 * t + phase), 0.0, 0.02 * std::cos(0.7 * t + phase));
    for (u32 b = 0; b < s.bones; ++b) {
        const f64 jy = s.y0 + (b + 0.5) * seg;
        const f64 ax = amplitude * std::sin(0.9 * t + phase + 0.6 * b);
        const f64 az = 0.6 * amplitude * std::cos(1.1 * t + 0.5 * phase + 0.4 * b);
        const M4 local = mul(translate(s.cx, jy, s.cz), mul(rotXZ(ax, az), translate(-s.cx, -jy, -s.cz)));
        parent = mul(parent, local);
        for (u32 i = 0; i < 16u; ++i) {
            out[b * 16u + i] = static_cast<f32>(parent.m[i]);
        }
    }
}

/// LBS of vertex v in f64 with the given f32 palette (the analytic reference of the motion gate).
inline void skinF64(const SkinMesh& s, const f32* pal, u32 v, f64 out[3]) {
    out[0] = out[1] = out[2] = 0.0;
    const f64 p[3] = {s.positions[v * 3u], s.positions[v * 3u + 1u], s.positions[v * 3u + 2u]};
    const SkinVertexWeights& w = s.weights[v];
    for (u32 k = 0; k < 4u; ++k) {
        if (w.weight[k] <= 0.f || w.bone[k] >= s.bones) {
            continue;
        }
        const f32* m = pal + w.bone[k] * 16u;
        for (u32 r = 0; r < 3u; ++r) {
            out[r] += w.weight[k] * (static_cast<f64>(m[r]) * p[0] + static_cast<f64>(m[4u + r]) * p[1] +
                                     static_cast<f64>(m[8u + r]) * p[2] + static_cast<f64>(m[12u + r]));
        }
    }
}

} // namespace sk_test
