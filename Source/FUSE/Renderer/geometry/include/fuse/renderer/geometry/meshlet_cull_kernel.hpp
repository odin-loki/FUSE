#pragma once

// WP-1.2 CPU reference meshlet cull (sphere-vs-frustum and backface cone), single-source
// (docs/compute-kernels.md), one meshlet per item. It is the reference the WP-1.3 / WP-5.x GPU
// culling shaders are checked against. The tests use no division or normalisation, only correctly
// rounded adds and multiplies (fixed evaluation order, `precise` in GLSL/Slang) plus one sqrt, which
// Vulkan does not round exactly: GPU decisions therefore equal these for every meshlet except those
// within a few ulp of a test boundary, and a parity test should skip meshlets whose margin
// (|lhs - rhs| of the deciding comparison) is below ~1e-5 of the operands' magnitude.
//
// Tests (all optional through CullView::flags):
//   frustum   culled when dot(plane.xyz, center) + plane.w < -radius for any of the 6 planes
//             (plane normals point inside and are unit length; see make_cull_view)
//   cone      meshoptimizer apex form: culled when dot(apex - camera, axis) >= cutoff * |apex - camera|
//             (the same as dot(normalize(apex - camera), axis) >= cutoff, without the division)
//   cone_s8   meshoptimizer sphere form on the snorm8 cone: culled when
//             dot(center - camera, axis8 * k) >= cutoff8 * k * |center - camera| + radius, k = kInv127
//             (the f32 constant 1/127; meshoptimizer's cutoff8 rounds up by one step, which absorbs it)
// A meshlet without a usable cone carries cutoff 1 (never culled). A meshlet whose triangles all have
// zero area carries axis 0 / cutoff 0 and is always cone-culled (it cannot produce a pixel).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/geometry/meshlet_types.hpp>

#include <cmath>

namespace fuse::renderer::geometry::cull_kernel {

inline constexpr const char* kName = "geometry_meshlet_cull";
inline constexpr u32 kWorkgroup = 64u;

inline constexpr f32 kInv127 = 1.f / 127.f;

inline constexpr u32 kTestFrustum = 1u << 0;
inline constexpr u32 kTestCone = 1u << 1;
inline constexpr u32 kTestConeS8 = 1u << 2;

/// Result bits per meshlet.
inline constexpr u32 kVisible = 1u << 0;
inline constexpr u32 kCulledFrustum = 1u << 1;
inline constexpr u32 kCulledCone = 1u << 2;
inline constexpr u32 kCulledConeS8 = 1u << 3;

struct CullView {
    f32 planes[6][4] = {}; ///< inward unit normals: inside when dot(n, p) + w >= 0
    f32 camera[3] = {0.f, 0.f, 0.f};
    u32 flags = kTestFrustum | kTestCone;
};

FUSE_HOST_DEVICE inline f32 dot3(const f32 a[3], const f32 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/// Returns kVisible, or the kCulled* bits of every test that rejected the meshlet.
FUSE_HOST_DEVICE inline u32 cull_meshlet(const MeshletRecord& m, const CullView& view) {
    u32 culled = 0u;
    if ((view.flags & kTestFrustum) != 0u) {
        for (u32 i = 0; i < 6u; ++i) {
            if (dot3(view.planes[i], m.center) + view.planes[i][3] < -m.radius) {
                culled |= kCulledFrustum;
                break;
            }
        }
    }
    if ((view.flags & kTestCone) != 0u) {
        const f32 d[3] = {m.cone_apex[0] - view.camera[0], m.cone_apex[1] - view.camera[1], m.cone_apex[2] - view.camera[2]};
        if (dot3(d, m.cone_axis) >= m.cone_cutoff * std::sqrt(dot3(d, d))) {
            culled |= kCulledCone;
        }
    }
    if ((view.flags & kTestConeS8) != 0u) {
        const f32 d[3] = {m.center[0] - view.camera[0], m.center[1] - view.camera[1], m.center[2] - view.camera[2]};
        const f32 axis[3] = {static_cast<f32>(m.cone_axis_s8[0]) * kInv127, static_cast<f32>(m.cone_axis_s8[1]) * kInv127,
                             static_cast<f32>(m.cone_axis_s8[2]) * kInv127};
        const f32 cutoff = static_cast<f32>(m.cone_cutoff_s8) * kInv127;
        if (dot3(d, axis) >= cutoff * std::sqrt(dot3(d, d)) + m.radius) {
            culled |= kCulledConeS8;
        }
    }
    return culled == 0u ? kVisible : culled;
}

struct Params {
    kernel::Span<const MeshletRecord> meshlets;
    CullView view{};
    kernel::Span<u32> out; ///< one result word per meshlet
};

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        p.out[idx.linear] = cull_meshlet(p.meshlets[idx.linear], p.view);
    }
};

inline kernel::KernelLaunch make_launch(u32 meshlet_count) {
    return kernel::KernelLaunch{kName, kernel::extent1(meshlet_count), {kWorkgroup, 1u, 1u}};
}

/// Frustum planes (inward, unit) from a column-major view-projection matrix (Vulkan clip space:
/// 0 <= z <= w), Gribb/Hartmann extraction; plus the camera position for the cone tests.
inline CullView make_cull_view(const f32 view_proj[16], const f32 camera[3], u32 flags = kTestFrustum | kTestCone) {
    CullView v{};
    auto row = [&](u32 r, u32 c) { return view_proj[c * 4u + r]; };
    const f32 rows[4][4] = {{row(0, 0), row(0, 1), row(0, 2), row(0, 3)},
                            {row(1, 0), row(1, 1), row(1, 2), row(1, 3)},
                            {row(2, 0), row(2, 1), row(2, 2), row(2, 3)},
                            {row(3, 0), row(3, 1), row(3, 2), row(3, 3)}};
    for (u32 c = 0; c < 4u; ++c) {
        v.planes[0][c] = rows[3][c] + rows[0][c]; // left
        v.planes[1][c] = rows[3][c] - rows[0][c]; // right
        v.planes[2][c] = rows[3][c] + rows[1][c]; // bottom
        v.planes[3][c] = rows[3][c] - rows[1][c]; // top
        v.planes[4][c] = rows[2][c];              // near (z >= 0)
        v.planes[5][c] = rows[3][c] - rows[2][c]; // far
    }
    for (auto& plane : v.planes) {
        const f32 len = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        const f32 inv = len > 0.f ? 1.f / len : 0.f;
        for (f32& c : plane) {
            c *= inv;
        }
    }
    for (u32 a = 0; a < 3u; ++a) {
        v.camera[a] = camera[a];
    }
    v.flags = flags;
    return v;
}

} // namespace fuse::renderer::geometry::cull_kernel
