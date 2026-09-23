#pragma once

// Device-safe (FUSE_HOST_DEVICE) camera + G-buffer view shared by the single-source screen-space effect
// kernels (hbao_kernel.hpp, ssr_kernel.hpp, ssgi_kernel.hpp — docs/compute-kernels.md). Everything the
// kernels call is defined inline here so the CPU backends and nvcc compile the same code.

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::ssfx {

/// Pinhole camera used by the CPU screen-space effect references (B5.7).
///
/// View space convention: camera at the origin looking down +Z, +X right, +Y down (matches pixel rows).
/// Pixel `(x, y)` covers `[x, x + 1) x [y, y + 1)`; its centre is `(x + 0.5, y + 0.5)`.
struct SsfxCamera {
    u32 width = 0;
    u32 height = 0;
    f32 fx = 1.f;
    f32 fy = 1.f;
    f32 cx = 0.f;
    f32 cy = 0.f;
    f32 near_z = 0.01f;

    /// Symmetric camera with vertical field of view `fovY` (radians) and square pixels.
    static SsfxCamera fromVerticalFov(u32 width, u32 height, f32 fovY);

    FUSE_HOST_DEVICE bool valid() const { return width > 0u && height > 0u && fx > 0.f && fy > 0.f && near_z > 0.f; }
    /// View-space point at linear depth `viewZ` behind continuous pixel coordinate `(px, py)`.
    FUSE_HOST_DEVICE math::Vec3 unproject(f32 px, f32 py, f32 viewZ) const { return rayDirection(px, py) * viewZ; }
    /// Unnormalised view ray through `(px, py)` with `z == 1`.
    FUSE_HOST_DEVICE math::Vec3 rayDirection(f32 px, f32 py) const { return {(px - cx) / fx, (py - cy) / fy, 1.f}; }
    /// Continuous pixel coordinates of a view-space point; false when behind the near plane.
    FUSE_HOST_DEVICE bool project(const math::Vec3& p, f32& px, f32& py) const {
        if (p.z < near_z) {
            return false;
        }
        px = fx * p.x / p.z + cx;
        py = fy * p.y / p.z + cy;
        return true;
    }
    FUSE_HOST_DEVICE bool inside(f32 px, f32 py) const {
        return px >= 0.f && py >= 0.f && px < static_cast<f32>(width) && py < static_cast<f32>(height);
    }
};

struct SsfxGBufferView;

/// Reconstruct a view-space normal from depth using the smaller one-sided difference per axis.
FUSE_HOST_DEVICE inline math::Vec3 ssfxReconstructNormal(const SsfxGBufferView& view, u32 x, u32 y);

/// Read-only G-buffer view consumed by the CPU HBAO / SSR references.
///
/// `depth` holds linear view-space Z per pixel (row-major, `width * height`); values <= 0 mean "sky / no
/// geometry". `normals` holds unit view-space normals; when null, normals are reconstructed from depth.
/// POD (pointers + scalars): it is passed by value into kernel params on every backend.
struct SsfxGBufferView {
    SsfxCamera camera{};
    const f32* depth = nullptr;
    const math::Vec3* normals = nullptr;
    /// `normals` are stored in the engine view convention (+Y up, -Z forward, as built by math::lookAt);
    /// `normalAt` converts them to this view's convention (a 180 degree rotation about X: y, z negated).
    bool engine_normals = false;

    FUSE_HOST_DEVICE bool valid() const { return camera.valid() && depth != nullptr; }
    FUSE_HOST_DEVICE u32 index(u32 x, u32 y) const { return y * camera.width + x; }
    FUSE_HOST_DEVICE f32 depthAt(u32 x, u32 y) const { return depth[index(x, y)]; }
    /// View-space position at the centre of pixel `(x, y)`.
    FUSE_HOST_DEVICE math::Vec3 positionAt(u32 x, u32 y) const {
        return camera.unproject(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f, depthAt(x, y));
    }
    /// View-space normal at pixel `(x, y)` — buffer value, or reconstructed from depth when absent.
    FUSE_HOST_DEVICE math::Vec3 normalAt(u32 x, u32 y) const {
        if (normals != nullptr) {
            const math::Vec3 n = normals[index(x, y)];
            return engine_normals ? math::Vec3{n.x, -n.y, -n.z} : n;
        }
        return ssfxReconstructNormal(*this, x, y);
    }
    /// Linear depth at continuous pixel coordinates — bilinear in 1/Z (exact on planes). <= 0 when off-screen.
    FUSE_HOST_DEVICE f32 sampleDepthBilinear(f32 px, f32 py) const;
};

namespace detail {

FUSE_HOST_DEVICE inline i32 clamp_index(i32 v, i32 hi) {
    return v < 0 ? 0 : (v > hi ? hi : v);
}

/// Smaller one-sided position difference along one axis (zero when neither neighbour has geometry).
FUSE_HOST_DEVICE inline math::Vec3 reconstruct_axis(const SsfxGBufferView& view, u32 x, u32 y, const math::Vec3& p,
                                                    bool horizontal) {
    const u32 w = view.camera.width;
    const u32 h = view.camera.height;
    const bool hasMinus = horizontal ? x > 0u : y > 0u;
    const bool hasPlus = horizontal ? x + 1u < w : y + 1u < h;
    math::Vec3 dMinus{};
    math::Vec3 dPlus{};
    bool okMinus = false;
    bool okPlus = false;
    if (hasMinus) {
        const u32 nx = horizontal ? x - 1u : x;
        const u32 ny = horizontal ? y : y - 1u;
        if (view.depthAt(nx, ny) > 0.f) {
            dMinus = p - view.positionAt(nx, ny);
            okMinus = true;
        }
    }
    if (hasPlus) {
        const u32 nx = horizontal ? x + 1u : x;
        const u32 ny = horizontal ? y : y + 1u;
        if (view.depthAt(nx, ny) > 0.f) {
            dPlus = view.positionAt(nx, ny) - p;
            okPlus = true;
        }
    }
    if (okMinus && okPlus) {
        return std::fabs(dMinus.z) <= std::fabs(dPlus.z) ? dMinus : dPlus;
    }
    return okMinus ? dMinus : dPlus;
}

} // namespace detail

FUSE_HOST_DEVICE inline f32 SsfxGBufferView::sampleDepthBilinear(f32 px, f32 py) const {
    if (!camera.inside(px, py)) {
        return 0.f;
    }
    const f32 fx0 = px - 0.5f;
    const f32 fy0 = py - 0.5f;
    const i32 maxX = static_cast<i32>(camera.width) - 1;
    const i32 maxY = static_cast<i32>(camera.height) - 1;
    const i32 x0 = static_cast<i32>(std::floor(fx0));
    const i32 y0 = static_cast<i32>(std::floor(fy0));
    const f32 tx = fx0 - static_cast<f32>(x0);
    const f32 ty = fy0 - static_cast<f32>(y0);
    const i32 xa = detail::clamp_index(x0, maxX);
    const i32 xb = detail::clamp_index(x0 + 1, maxX);
    const i32 ya = detail::clamp_index(y0, maxY);
    const i32 yb = detail::clamp_index(y0 + 1, maxY);
    const f32 d00 = depthAt(static_cast<u32>(xa), static_cast<u32>(ya));
    const f32 d10 = depthAt(static_cast<u32>(xb), static_cast<u32>(ya));
    const f32 d01 = depthAt(static_cast<u32>(xa), static_cast<u32>(yb));
    const f32 d11 = depthAt(static_cast<u32>(xb), static_cast<u32>(yb));
    if (d00 <= 0.f || d10 <= 0.f || d01 <= 0.f || d11 <= 0.f) {
        // Sky in the footprint — fall back to the nearest tap.
        return depthAt(static_cast<u32>(detail::clamp_index(static_cast<i32>(px), maxX)),
                       static_cast<u32>(detail::clamp_index(static_cast<i32>(py), maxY)));
    }
    const f32 top = (1.f - tx) / d00 + tx / d10;
    const f32 bottom = (1.f - tx) / d01 + tx / d11;
    const f32 inv = (1.f - ty) * top + ty * bottom;
    return inv > 0.f ? 1.f / inv : 0.f;
}

FUSE_HOST_DEVICE inline math::Vec3 ssfxReconstructNormal(const SsfxGBufferView& view, u32 x, u32 y) {
    const math::Vec3 p = view.positionAt(x, y);
    const math::Vec3 ddx = detail::reconstruct_axis(view, x, y, p, true);
    const math::Vec3 ddy = detail::reconstruct_axis(view, x, y, p, false);
    // +X right, +Y down: cross(ddy, ddx) faces the camera (-Z) for a fronto-parallel surface.
    math::Vec3 n = math::cross(ddy, ddx).normalized();
    if (n.dot(p) > 0.f) {
        n = n * -1.f;
    }
    return n;
}

} // namespace fuse::ssfx
