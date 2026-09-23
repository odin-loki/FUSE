#include <fuse/compute/screen_space_contact.hpp>
#include <fuse/compute/screen_space_effects.hpp>

#include <fuse/ssfx/hbao.hpp>
#include <fuse/ssfx/ssfx_view.hpp>
#include <fuse/ssfx/ssgi.hpp>
#include <fuse/ssfx/ssr.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::compute {

namespace {

f32 clamp01(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

f32 luminance(const math::Vec3& c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

/// Intrinsics of a column-major perspective projection (math::perspective layout: clip.w = -z_view).
/// Pixel rows grow downwards, so view +Y (up) maps to decreasing rows.
bool cameraFromProjection(const f32 (&proj)[16], u32 width, u32 height, ssfx::SsfxCamera& camera) {
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

/// G-buffer view over the host surfaces; engine view-space normals are converted to the ssfx convention
/// (+Y down, +Z forward — a 180 degree rotation about X) into `normalStorage`.
bool makeView(const f32 (&proj)[16], u32 width, u32 height, const void* depthSurface, const void* normalSurface,
              std::vector<math::Vec3>& normalStorage, ssfx::SsfxGBufferView& view) {
    if (depthSurface == nullptr || !cameraFromProjection(proj, width, height, view.camera)) {
        return false;
    }
    view.depth = static_cast<const f32*>(depthSurface);
    view.normals = nullptr;
    if (normalSurface != nullptr) {
        const auto* normals = static_cast<const math::Vec3*>(normalSurface);
        normalStorage.resize(static_cast<size_t>(width) * height);
        for (size_t i = 0; i < normalStorage.size(); ++i) {
            normalStorage[i] = math::Vec3{normals[i].x, -normals[i].y, -normals[i].z};
        }
        view.normals = normalStorage.data();
    }
    return view.valid();
}

ssfx::HbaoParams toHbao(const SSAOParams& params) {
    ssfx::HbaoParams hbao{};
    hbao.radius = params.radius;
    hbao.bias = params.bias;
    hbao.directions = params.directions;
    hbao.steps_per_dir = params.steps_per_dir;
    hbao.strength = params.strength;
    hbao.max_radius_px = params.max_radius_px;
    return hbao;
}

ssfx::SsrParams toSsr(const SSRParams& params) {
    ssfx::SsrParams ssr{};
    ssr.max_steps = params.max_steps;
    ssr.stride_px = params.ray_step_size;
    ssr.thickness = params.thickness;
    ssr.max_distance = params.max_distance;
    ssr.fade_screen_edge = params.fade_screen_edge;
    ssr.refine_steps = params.refine_steps;
    return ssr;
}

ssfx::SsgiParams toSsgi(const SSGIParams& params) {
    ssfx::SsgiParams ssgi{};
    ssgi.sample_sqrt = params.sample_sqrt;
    ssgi.bounces = params.max_bounces;
    ssgi.max_steps = params.max_steps;
    ssgi.stride_px = params.ray_step_size;
    ssgi.thickness = params.thickness;
    ssgi.max_distance = params.max_distance;
    ssgi.intensity = params.intensity;
    return ssgi;
}

/// Confidence multiplier for a mirror trace standing in for a glossy lobe of `roughness`.
f32 ssrGlossFade(const SSRParams& params, f32 hitDistance, f32 roughness) {
    const f32 r = clamp01(roughness);
    const f32 hardened = r <= params.contact_roughness_floor ? r : ssr_contact_harden_roughness(hitDistance, r, params);
    return clamp01(1.f - hardened);
}

/// SSR at one pixel including the roughness mask and gloss fade; returns rgb + confidence.
math::Vec4 ssrPixel(const ssfx::SsfxGBufferView& view, const SSRParams& params, const ssfx::SsrParams& trace, u32 x,
                    u32 y) {
    const auto* sceneColor = static_cast<const math::Vec3*>(params.scene_color_surface);
    const auto* roughness = static_cast<const f32*>(params.roughness_surface);
    const f32 r = roughness != nullptr ? roughness[view.index(x, y)] : 0.f;
    if (r >= 1.f) {
        return {};
    }
    const ssfx::SsrHit hit = ssfx::ssrTracePixel(view, sceneColor, trace, x, y);
    if (!hit.hit) {
        return {};
    }
    const f32 fade = roughness != nullptr ? ssrGlossFade(params, hit.distance, r) : 1.f;
    return math::Vec4{hit.color, hit.confidence * fade};
}

} // namespace

f32 ssao_center_sample(const SSAOParams& params) {
    std::vector<math::Vec3> normals;
    ssfx::SsfxGBufferView view{};
    if (!validate_ssao_params(params) ||
        !makeView(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, normals,
                  view)) {
        return 1.f;
    }
    return ssfx::hbaoPixelVisibility(view, toHbao(params), params.width / 2u, params.height / 2u);
}

f32 ssr_center_sample(const SSRParams& params) {
    std::vector<math::Vec3> normals;
    ssfx::SsfxGBufferView view{};
    if (!validate_ssr_params(params) || params.scene_color_surface == nullptr ||
        !makeView(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, normals,
                  view)) {
        return 0.f;
    }
    const math::Vec4 c = ssrPixel(view, params, toSsr(params), params.width / 2u, params.height / 2u);
    return luminance(math::Vec3{c.x, c.y, c.z}) * c.w;
}

f32 ssgi_center_sample(const SSGIParams& params) {
    std::vector<math::Vec3> normals;
    ssfx::SsfxGBufferView view{};
    if (!validate_ssgi_params(params) || params.scene_color_surface == nullptr ||
        !makeView(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, normals,
                  view)) {
        return 0.f;
    }
    const auto* sceneColor = static_cast<const math::Vec3*>(params.scene_color_surface);
    const auto* albedo = static_cast<const math::Vec3*>(params.albedo_surface);
    const u32 cx = params.width / 2u;
    const u32 cy = params.height / 2u;
    if (params.max_bounces <= 1u) {
        const ssfx::SsgiParams ssgi = toSsgi(params);
        if (ssgi.bounces == 0u) {
            return 0.f;
        }
        const math::Vec3 gathered = ssfx::ssgiPixelGather(view, sceneColor, ssgi, cx, cy);
        const math::Vec3 a = albedo != nullptr ? albedo[view.index(cx, cy)] : math::Vec3{1.f, 1.f, 1.f};
        return params.intensity * luminance(math::Vec3{a.x * gathered.x, a.y * gathered.y, a.z * gathered.z});
    }
    // Later bounces need the whole frame's previous bounce.
    std::vector<math::Vec3> indirect(static_cast<size_t>(params.width) * params.height);
    if (!ssfx::computeSsgiCpu(view, sceneColor, albedo, toSsgi(params), indirect.data())) {
        return 0.f;
    }
    return luminance(indirect[view.index(cx, cy)]);
}

bool launch_ssao_cpu(const SSAOParams& params) {
    std::vector<math::Vec3> normals;
    ssfx::SsfxGBufferView view{};
    if (!validate_ssao_params(params) || params.ao_out_surface == nullptr ||
        !makeView(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, normals,
                  view)) {
        return false;
    }
    auto* out = static_cast<f32*>(params.ao_out_surface);
    std::vector<f32> raw(static_cast<size_t>(params.width) * params.height);
    ssfx::computeHbaoCpu(view, toHbao(params), raw.data());
    if (!params.enable_blur) {
        std::copy(raw.begin(), raw.end(), out);
        return true;
    }

    // Cross-bilateral 5x5 blur: taps off the centre's tangent plane or with diverging normals are rejected.
    constexpr i32 kRadius = 2;
    const i32 w = static_cast<i32>(params.width);
    const i32 h = static_cast<i32>(params.height);
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            const u32 ci = view.index(static_cast<u32>(x), static_cast<u32>(y));
            const f32 cz = view.depth[ci];
            if (cz <= 0.f) {
                out[ci] = raw[ci];
                continue;
            }
            const math::Vec3 cp = view.positionAt(static_cast<u32>(x), static_cast<u32>(y));
            const math::Vec3 cn = view.normalAt(static_cast<u32>(x), static_cast<u32>(y)).normalized();
            f32 sum = raw[ci];
            f32 weight = 1.f;
            for (i32 dy = -kRadius; dy <= kRadius; ++dy) {
                for (i32 dx = -kRadius; dx <= kRadius; ++dx) {
                    const i32 nx = x + dx;
                    const i32 ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h) {
                        continue;
                    }
                    const u32 ni = view.index(static_cast<u32>(nx), static_cast<u32>(ny));
                    if (view.depth[ni] <= 0.f) {
                        continue;
                    }
                    const math::Vec3 np = view.positionAt(static_cast<u32>(nx), static_cast<u32>(ny));
                    const math::Vec3 nn = view.normalAt(static_cast<u32>(nx), static_cast<u32>(ny)).normalized();
                    const f32 planeDelta = std::fabs(cn.dot(np - cp)) / cz;
                    const f32 wgt = ssao_blur_weight(0.f, planeDelta, 1.f, cn.dot(nn), params);
                    sum += wgt * raw[ni];
                    weight += wgt;
                }
            }
            out[ci] = sum / weight;
        }
    }
    return true;
}

bool launch_ssr_cpu(const SSRParams& params) {
    std::vector<math::Vec3> normals;
    ssfx::SsfxGBufferView view{};
    if (!validate_ssr_params(params) || params.scene_color_surface == nullptr || params.ssr_out_surface == nullptr ||
        !makeView(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, normals,
                  view)) {
        return false;
    }
    auto* out = static_cast<math::Vec4*>(params.ssr_out_surface);
    const ssfx::SsrParams trace = toSsr(params);
    for (u32 y = 0u; y < params.height; ++y) {
        for (u32 x = 0u; x < params.width; ++x) {
            out[view.index(x, y)] = ssrPixel(view, params, trace, x, y);
        }
    }
    return true;
}

bool launch_ssgi_cpu(const SSGIParams& params) {
    std::vector<math::Vec3> normals;
    ssfx::SsfxGBufferView view{};
    if (!validate_ssgi_params(params) || params.scene_color_surface == nullptr ||
        params.ssgi_out_surface == nullptr ||
        !makeView(params.proj, params.width, params.height, params.depth_surface, params.normal_surface, normals,
                  view)) {
        return false;
    }
    return ssfx::computeSsgiCpu(view, static_cast<const math::Vec3*>(params.scene_color_surface),
                                static_cast<const math::Vec3*>(params.albedo_surface), toSsgi(params),
                                static_cast<math::Vec3*>(params.ssgi_out_surface));
}

} // namespace fuse::compute
