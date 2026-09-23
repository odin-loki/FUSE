#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

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

    bool valid() const { return width > 0u && height > 0u && fx > 0.f && fy > 0.f && near_z > 0.f; }
    /// View-space point at linear depth `viewZ` behind continuous pixel coordinate `(px, py)`.
    math::Vec3 unproject(f32 px, f32 py, f32 viewZ) const;
    /// Unnormalised view ray through `(px, py)` with `z == 1`.
    math::Vec3 rayDirection(f32 px, f32 py) const;
    /// Continuous pixel coordinates of a view-space point; false when behind the near plane.
    bool project(const math::Vec3& p, f32& px, f32& py) const;
    bool inside(f32 px, f32 py) const;
};

/// Read-only G-buffer view consumed by the CPU HBAO / SSR references.
///
/// `depth` holds linear view-space Z per pixel (row-major, `width * height`); values <= 0 mean "sky / no
/// geometry". `normals` holds unit view-space normals; when null, normals are reconstructed from depth.
struct SsfxGBufferView {
    SsfxCamera camera{};
    const f32* depth = nullptr;
    const math::Vec3* normals = nullptr;

    bool valid() const { return camera.valid() && depth != nullptr; }
    u32 index(u32 x, u32 y) const { return y * camera.width + x; }
    f32 depthAt(u32 x, u32 y) const { return depth[index(x, y)]; }
    /// View-space position at the centre of pixel `(x, y)`.
    math::Vec3 positionAt(u32 x, u32 y) const;
    /// View-space normal at pixel `(x, y)` — buffer value, or reconstructed from depth when absent.
    math::Vec3 normalAt(u32 x, u32 y) const;
    /// Linear depth at continuous pixel coordinates — bilinear in 1/Z (exact on planes). <= 0 when off-screen.
    f32 sampleDepthBilinear(f32 px, f32 py) const;
};

/// Reconstruct a view-space normal from depth using the smaller one-sided difference per axis.
math::Vec3 ssfxReconstructNormal(const SsfxGBufferView& view, u32 x, u32 y);

} // namespace fuse::renderer
