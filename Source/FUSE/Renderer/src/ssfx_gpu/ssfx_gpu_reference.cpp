// WP-6.3 screen-space fallback: see include/fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp.
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/ssfx/ssgi_kernel.hpp>

#include <cmath>

namespace fuse::renderer::ssfx_gpu {

bool camera_from_projection(const f32 (&proj)[16], u32 width, u32 height, ssfx::SsfxCamera& camera) {
    if (width == 0u || height == 0u || !(proj[0] > 0.f) || !(proj[5] > 0.f) || std::fabs(proj[11] + 1.f) > 1e-4f) {
        return false;
    }
    camera.width = width;
    camera.height = height;
    camera.fx = 0.5f * static_cast<f32>(width) * proj[0];
    camera.fy = 0.5f * static_cast<f32>(height) * proj[5];
    camera.cx = 0.5f * static_cast<f32>(width) * (1.f - proj[8]);
    camera.cy = 0.5f * static_cast<f32>(height) * (1.f + proj[9]);
    // Vulkan depth range: z_ndc = 0 at the near plane, so near = proj[14] / proj[10].
    const f32 nearZ = proj[10] != 0.f ? proj[14] / proj[10] : 0.f;
    camera.near_z = nearZ > 0.f ? nearZ : 0.01f;
    return camera.valid();
}

bool resolve_constants(const SsfxGpuSettings& settings, const SsfxCameraDesc& camera, const f32 (&ambient)[3],
                       u32 width, u32 height, SsfxFrameConstants& out) {
    ssfx::SsfxCamera cam{};
    if (!camera_from_projection(camera.proj, width, height, cam) || !(camera.nearPlane > 0.f)) {
        return false;
    }
    SsfxFrameConstants c{};
    c.width = width;
    c.height = height;
    c.flags = 0u;
    if (settings.ao) {
        c.flags |= kSsfxFlagAo;
    }
    if (settings.ssr) {
        c.flags |= kSsfxFlagSsr;
    }
    if (settings.ssgi) {
        c.flags |= kSsfxFlagSsgi;
    }
    if (camera.reversedZ) {
        c.flags |= kSsfxFlagReversedZ;
    }
    if (settings.multiBounce) {
        c.flags |= kSsfxFlagMultiBounce;
    }
    if (settings.ssrRoughness) {
        c.flags |= kSsfxFlagSsrRoughness;
    }
    if (settings.contact.enabled) {
        c.flags |= kSsfxFlagSsrContact;
    }
    c.fx = cam.fx;
    c.fy = cam.fy;
    c.cx = cam.cx;
    c.cy = cam.cy;
    c.nearZ = cam.near_z;
    c.nearPlane = camera.nearPlane;
    c.farPlane = camera.farPlane;
    for (u32 col = 0; col < 3u; ++col) {
        for (u32 row = 0; row < 3u; ++row) {
            c.viewRot[col * 4u + row] = camera.view[col * 4u + row];
        }
    }
    c.ambient[0] = ambient[0];
    c.ambient[1] = ambient[1];
    c.ambient[2] = ambient[2];

    const ssfx::gtao_kernel::Params g = ssfx::gtao_kernel::make_params(ssfx::SsfxGBufferView{}, settings.gtao, nullptr);
    c.aoRadius = g.gtao.radius;
    c.aoFalloff = g.gtao.falloff;
    c.aoBias = g.gtao.bias;
    c.aoStrength = g.gtao.strength;
    c.aoMaxRadiusPx = g.gtao.max_radius_px;
    c.aoSlices = g.gtao.slices;
    c.aoSteps = g.gtao.steps;
    c.aoFrame = g.gtao.frame;
    if (g.gtao.jitter) {
        c.flags |= kSsfxFlagAoJitter;
    }
    for (u32 k = 0; k < kSsfxMaxSlices; ++k) {
        c.aoSliceCos[k] = g.slice_cos[k];
        c.aoSliceSin[k] = g.slice_sin[k];
    }

    const ssfx::SsrParams s = ssfx::ssr_kernel::clamp_params(settings.ssr_params);
    c.ssrMaxSteps = s.max_steps;
    c.ssrRefineSteps = s.refine_steps;
    c.ssrStride = s.stride_px;
    c.ssrThickness = s.thickness;
    c.ssrMaxDistance = s.max_distance;
    c.ssrFadeEdge = s.fade_screen_edge;
    c.ssrContactDistance = settings.contact.distance;
    c.ssrContactFloor = settings.contact.roughness_floor;
    c.ssrContactExponent = settings.contact.exponent;

    const ssfx::SsgiParams gi = ssfx::ssgi_kernel::clamp_params(settings.ssgi_params);
    c.giSampleSqrt = gi.sample_sqrt;
    c.giBounces = gi.bounces;
    c.giMaxSteps = gi.max_steps;
    c.giRefineSteps = gi.refine_steps;
    c.giStride = gi.stride_px;
    c.giThickness = gi.thickness;
    c.giMaxDistance = gi.max_distance;
    c.giIntensity = gi.intensity;
    out = c;
    return true;
}

ssfx::SsfxGBufferView prepared_view(const SsfxFrameConstants& c, const std::vector<f32>& depth,
                                    const std::vector<math::Vec3>& normals) {
    ssfx::SsfxGBufferView view{};
    view.camera = camera_of(c);
    view.depth = depth.data();
    view.normals = normals.data();
    view.engine_normals = false;
    return view;
}

bool computeGtaoCpu(const ssfx::SsfxGBufferView& view, const ssfx::GtaoParams& params, f32* visibilityOut,
                    kernel::Backend backend) {
    if (!view.valid() || visibilityOut == nullptr) {
        return false;
    }
    return kernel::launch(backend, ssfx::gtao_kernel::make_launch(view), ssfx::gtao_kernel::Kernel{},
                          ssfx::gtao_kernel::make_params(view, params, visibilityOut))
        .ok;
}

bool reference_frame(const SsfxGpuSettings& settings, const SsfxFrameConstants& c, const SsfxPreparedFrame& in,
                     SsfxReferenceFrame& out, kernel::Backend backend) {
    const usize n = static_cast<usize>(in.width) * in.height;
    if (n == 0u || in.width != c.width || in.height != c.height || in.prepared.size() != n || in.normal.size() != n ||
        in.radiance.size() != n || in.litAlpha.size() != n || in.albedo.size() != n || in.diffuse.size() != n) {
        return false;
    }
    std::vector<f32> depth(n);
    std::vector<f32> roughness(n);
    for (usize i = 0; i < n; ++i) {
        depth[i] = in.prepared[i].x;
        roughness[i] = in.prepared[i].y;
    }
    const ssfx::SsfxGBufferView view = prepared_view(c, depth, in.normal);
    out.ao.assign(n, 1.f);
    out.ssr.assign(n, math::Vec4{});
    out.gi.assign(n, math::Vec3{});
    out.composed.assign(n, math::Vec4{});
    if (settings.ao && !computeGtaoCpu(view, settings.gtao, out.ao.data(), backend)) {
        return false;
    }
    if (settings.ssr) {
        ssfx::ssr_kernel::Params kp{};
        kp.view = view;
        kp.scene_color = in.radiance.data();
        kp.trace = settings.ssr_params;
        kp.roughness = settings.ssrRoughness ? roughness.data() : nullptr;
        kp.contact = settings.contact;
        kp.rgba_out = out.ssr.data();
        if (!kernel::launch(backend, ssfx::ssr_kernel::make_launch(view), ssfx::ssr_kernel::Kernel{}, kp).ok) {
            return false;
        }
    }
    if (settings.ssgi &&
        !ssfx::computeSsgiCpu(view, in.radiance.data(), in.diffuse.data(), settings.ssgi_params, out.gi.data(), backend)) {
        return false;
    }
    for (u32 y = 0; y < in.height; ++y) {
        for (u32 x = 0; x < in.width; ++x) {
            const usize i = static_cast<usize>(y) * in.width + x;
            out.composed[i] = compose_pixel(c, x, y, in.prepared[i], in.normal[i], in.albedo[i], in.radiance[i],
                                            in.litAlpha[i], out.ao[i], out.ssr[i], out.gi[i]);
        }
    }
    return true;
}

} // namespace fuse::renderer::ssfx_gpu
