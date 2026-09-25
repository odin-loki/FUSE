#include <fuse/ssfx/ssfx_view.hpp>

#include <cmath>

// The device-safe view / camera math is inline in ssfx_view.hpp (shared with the CUDA kernels); only the
// host-side camera factory lives here.

namespace fuse::ssfx {

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

} // namespace fuse::ssfx
