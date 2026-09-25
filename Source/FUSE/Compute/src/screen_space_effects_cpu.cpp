// CPU entry points of SSAO / SSR / SSGI. All per-pixel math lives once in the single-source kernels
// (fuse/ssfx/{hbao,ssr,ssgi}_kernel.hpp + the AO blur in fuse/compute/screen_space_kernels.hpp), shared with
// kernels/{ssao,ssr,ssgi}.cu; this TU validates the engine-facing params and launches the bodies through
// kernel::launch on the CPU backends.

#include <fuse/compute/screen_space_contact.hpp>
#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute/screen_space_kernels.hpp>
#include <fuse/compute_kernel/launch.hpp>

#include <fuse/ssfx/hbao.hpp>
#include <fuse/ssfx/ssgi.hpp>

#include <vector>

namespace fuse::compute {

namespace ssk = screen_space_kernels;

namespace {

f32 luminance(const math::Vec3& c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

bool ssaoView(const SSAOParams& params, ssfx::SsfxGBufferView& view) {
    return validate_ssao_params(params) &&
           ssk::make_view(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, view);
}

bool ssrView(const SSRParams& params, ssfx::SsfxGBufferView& view) {
    return validate_ssr_params(params) && params.scene_color_surface != nullptr &&
           ssk::make_view(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, view);
}

bool ssgiView(const SSGIParams& params, ssfx::SsfxGBufferView& view) {
    return validate_ssgi_params(params) && params.scene_color_surface != nullptr &&
           ssk::make_view(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, view);
}

} // namespace

f32 ssao_center_sample(const SSAOParams& params) {
    ssfx::SsfxGBufferView view{};
    if (!ssaoView(params, view)) {
        return 1.f;
    }
    return ssfx::hbaoPixelVisibility(view, ssk::to_hbao(params), params.width / 2u, params.height / 2u);
}

f32 ssr_center_sample(const SSRParams& params) {
    ssfx::SsfxGBufferView view{};
    if (!ssrView(params, view)) {
        return 0.f;
    }
    const ssfx::ssr_kernel::Params kp =
        ssk::make_ssr_params(view, params, static_cast<const math::Vec3*>(params.scene_color_surface),
                             static_cast<const f32*>(params.roughness_surface), nullptr);
    const math::Vec4 c = ssfx::ssr_kernel::shade_pixel(kp, params.width / 2u, params.height / 2u);
    return luminance(math::Vec3{c.x, c.y, c.z}) * c.w;
}

f32 ssgi_center_sample(const SSGIParams& params) {
    ssfx::SsfxGBufferView view{};
    if (!ssgiView(params, view)) {
        return 0.f;
    }
    const auto* sceneColor = static_cast<const math::Vec3*>(params.scene_color_surface);
    const auto* albedo = static_cast<const math::Vec3*>(params.albedo_surface);
    const u32 cx = params.width / 2u;
    const u32 cy = params.height / 2u;
    if (params.max_bounces <= 1u) {
        const ssfx::SsgiParams ssgi = ssk::to_ssgi(params);
        if (ssgi.bounces == 0u) {
            return 0.f;
        }
        const math::Vec3 gathered = ssfx::ssgiPixelGather(view, sceneColor, ssgi, cx, cy);
        const math::Vec3 a = albedo != nullptr ? albedo[view.index(cx, cy)] : math::Vec3{1.f, 1.f, 1.f};
        return params.intensity * luminance(math::Vec3{a.x * gathered.x, a.y * gathered.y, a.z * gathered.z});
    }
    // Later bounces need the whole frame's previous bounce.
    std::vector<math::Vec3> indirect(static_cast<size_t>(params.width) * params.height);
    if (!ssfx::computeSsgiCpu(view, sceneColor, albedo, ssk::to_ssgi(params), indirect.data())) {
        return 0.f;
    }
    return luminance(indirect[view.index(cx, cy)]);
}

bool launch_ssao_cpu_backend(kernel::Backend backend, const SSAOParams& params) {
    ssfx::SsfxGBufferView view{};
    if (params.ao_out_surface == nullptr || !ssaoView(params, view)) {
        return false;
    }
    auto* out = static_cast<f32*>(params.ao_out_surface);
    if (!params.enable_blur) {
        return ssfx::computeHbaoCpu(view, ssk::to_hbao(params), out, backend);
    }
    std::vector<f32> raw(static_cast<size_t>(params.width) * params.height);
    if (!ssfx::computeHbaoCpu(view, ssk::to_hbao(params), raw.data(), backend)) {
        return false;
    }
    const ssk::ao_blur::Params blur{view, ssk::ao_blur::thresholds(params), raw.data(), out};
    return kernel::launch(backend, ssk::ao_blur::make_launch(view), ssk::ao_blur::Kernel{}, blur).ok;
}

bool launch_ssr_cpu_backend(kernel::Backend backend, const SSRParams& params) {
    ssfx::SsfxGBufferView view{};
    if (params.ssr_out_surface == nullptr || !ssrView(params, view)) {
        return false;
    }
    const ssfx::ssr_kernel::Params kp = ssk::make_ssr_params(
        view, params, static_cast<const math::Vec3*>(params.scene_color_surface),
        static_cast<const f32*>(params.roughness_surface), static_cast<math::Vec4*>(params.ssr_out_surface));
    return kernel::launch(backend, ssfx::ssr_kernel::make_launch(view), ssfx::ssr_kernel::Kernel{}, kp).ok;
}

bool launch_ssgi_cpu_backend(kernel::Backend backend, const SSGIParams& params) {
    ssfx::SsfxGBufferView view{};
    if (params.ssgi_out_surface == nullptr || !ssgiView(params, view)) {
        return false;
    }
    return ssfx::computeSsgiCpu(view, static_cast<const math::Vec3*>(params.scene_color_surface),
                                static_cast<const math::Vec3*>(params.albedo_surface), ssk::to_ssgi(params),
                                static_cast<math::Vec3*>(params.ssgi_out_surface), backend);
}

bool launch_ssao_cpu(const SSAOParams& params) {
    return launch_ssao_cpu_backend(kernel::Backend::CpuReference, params);
}

bool launch_ssr_cpu(const SSRParams& params) {
    return launch_ssr_cpu_backend(kernel::Backend::CpuReference, params);
}

bool launch_ssgi_cpu(const SSGIParams& params) {
    return launch_ssgi_cpu_backend(kernel::Backend::CpuReference, params);
}

} // namespace fuse::compute
