#include <fuse/ssfx/ssr.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/ssfx/ssr_kernel.hpp>

// The march, bisection and fades live once in fuse/ssfx/ssr_kernel.hpp (FUSE_HOST_DEVICE, shared with
// Compute/kernels/ssr.cu); this TU adapts the public scalar API and launches the full-frame kernel.

namespace fuse::ssfx {

SsrParams clampSsrParams(const SsrParams& raw) {
    return ssr_kernel::clamp_params(raw);
}

math::Vec3 ssrReflect(const math::Vec3& incident, const math::Vec3& n) {
    return ssr_kernel::reflect(incident, n);
}

SsrHit ssrTracePixel(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& params, u32 x,
                     u32 y) {
    return ssr_kernel::trace_pixel(view, sceneColor, params, x, y);
}

SsrHit ssrTraceRay(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& rawParams, u32 x,
                   u32 y, const math::Vec3& direction) {
    return ssr_kernel::trace_ray(view, sceneColor, rawParams, x, y, direction);
}

bool computeSsrCpu(const SsfxGBufferView& view, const math::Vec3* sceneColor, const SsrParams& params,
                   const f32* reflectiveMask, math::Vec3* colorOut, f32* confidenceOut, kernel::Backend backend) {
    if (!view.valid() || sceneColor == nullptr) {
        return false;
    }
    ssr_kernel::Params kp{};
    kp.view = view;
    kp.scene_color = sceneColor;
    kp.trace = params;
    kp.reflective_mask = reflectiveMask;
    kp.color_out = colorOut;
    kp.confidence_out = confidenceOut;
    return kernel::launch(backend, ssr_kernel::make_launch(view), ssr_kernel::Kernel{}, kp).ok;
}

} // namespace fuse::ssfx
