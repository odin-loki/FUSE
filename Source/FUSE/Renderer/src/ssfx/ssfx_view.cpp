#include <fuse/renderer/ssfx/ssfx_view.hpp>

#include <cmath>

namespace fuse::renderer {

SsfxCamera SsfxCamera::fromVerticalFov(u32 width, u32 height, f32 fovY) {
    SsfxCamera camera{};
    camera.width = width;
    camera.height = height;
    const f32 focal = height > 0u ? 0.5f * static_cast<f32>(height) / std::tan(0.5f * fovY) : 1.f;
    camera.fx = focal;
    camera.fy = focal;
    camera.cx = 0.5f * static_cast<f32>(width);
    camera.cy = 0.5f * static_cast<f32>(height);
    return camera;
}

math::Vec3 SsfxCamera::rayDirection(f32 px, f32 py) const {
    return {(px - cx) / fx, (py - cy) / fy, 1.f};
}

math::Vec3 SsfxCamera::unproject(f32 px, f32 py, f32 viewZ) const {
    return rayDirection(px, py) * viewZ;
}

bool SsfxCamera::project(const math::Vec3& p, f32& px, f32& py) const {
    if (p.z < near_z) {
        return false;
    }
    px = fx * p.x / p.z + cx;
    py = fy * p.y / p.z + cy;
    return true;
}

bool SsfxCamera::inside(f32 px, f32 py) const {
    return px >= 0.f && py >= 0.f && px < static_cast<f32>(width) && py < static_cast<f32>(height);
}

math::Vec3 SsfxGBufferView::positionAt(u32 x, u32 y) const {
    return camera.unproject(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f, depthAt(x, y));
}

math::Vec3 SsfxGBufferView::normalAt(u32 x, u32 y) const {
    if (normals != nullptr) {
        return normals[index(x, y)];
    }
    return ssfxReconstructNormal(*this, x, y);
}

f32 SsfxGBufferView::sampleDepthBilinear(f32 px, f32 py) const {
    if (!camera.inside(px, py)) {
        return 0.f;
    }
    const f32 fx0 = px - 0.5f;
    const f32 fy0 = py - 0.5f;
    const i32 maxX = static_cast<i32>(camera.width) - 1;
    const i32 maxY = static_cast<i32>(camera.height) - 1;
    i32 x0 = static_cast<i32>(std::floor(fx0));
    i32 y0 = static_cast<i32>(std::floor(fy0));
    const f32 tx = fx0 - static_cast<f32>(x0);
    const f32 ty = fy0 - static_cast<f32>(y0);
    auto clampI = [](i32 v, i32 hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    const i32 xa = clampI(x0, maxX);
    const i32 xb = clampI(x0 + 1, maxX);
    const i32 ya = clampI(y0, maxY);
    const i32 yb = clampI(y0 + 1, maxY);
    const f32 d00 = depthAt(static_cast<u32>(xa), static_cast<u32>(ya));
    const f32 d10 = depthAt(static_cast<u32>(xb), static_cast<u32>(ya));
    const f32 d01 = depthAt(static_cast<u32>(xa), static_cast<u32>(yb));
    const f32 d11 = depthAt(static_cast<u32>(xb), static_cast<u32>(yb));
    if (d00 <= 0.f || d10 <= 0.f || d01 <= 0.f || d11 <= 0.f) {
        // Sky in the footprint — fall back to the nearest tap.
        return depthAt(static_cast<u32>(clampI(static_cast<i32>(px), maxX)),
                       static_cast<u32>(clampI(static_cast<i32>(py), maxY)));
    }
    const f32 top = (1.f - tx) / d00 + tx / d10;
    const f32 bottom = (1.f - tx) / d01 + tx / d11;
    const f32 inv = (1.f - ty) * top + ty * bottom;
    return inv > 0.f ? 1.f / inv : 0.f;
}

math::Vec3 ssfxReconstructNormal(const SsfxGBufferView& view, u32 x, u32 y) {
    const u32 w = view.camera.width;
    const u32 h = view.camera.height;
    const math::Vec3 p = view.positionAt(x, y);

    auto pickAxis = [&](bool horizontal) -> math::Vec3 {
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
    };

    const math::Vec3 ddx = pickAxis(true);
    const math::Vec3 ddy = pickAxis(false);
    // +X right, +Y down: cross(ddy, ddx) faces the camera (-Z) for a fronto-parallel surface.
    math::Vec3 n = math::cross(ddy, ddx).normalized();
    if (n.dot(p) > 0.f) {
        n = n * -1.f;
    }
    return n;
}

} // namespace fuse::renderer
