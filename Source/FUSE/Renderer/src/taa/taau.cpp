#include <fuse/renderer/taa/taau.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/math/mat.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

template <typename T>
kernel::Span<const T> constSpan(const T* data, usize count) {
    return kernel::make_span(data, data != nullptr ? static_cast<u32>(count) : 0u);
}

template <typename T>
kernel::Span<T> spanOf(std::vector<T>& v) {
    return kernel::make_span(v.data(), static_cast<u32>(v.size()));
}

/// cur view -> prev view (3x4 row-major) = prev.view * inverse(cur.view). False when the views are not rigid.
bool currentToPreviousView(const UpscaleCamera& cur, const UpscaleCamera& prev, f32 out[12]) {
    math::Mat4 inv{};
    if (!math::tryInverseAffine(cur.view, inv)) {
        return false;
    }
    const math::Mat4 m = prev.view * inv;
    for (u32 row = 0; row < 3u; ++row) {
        for (u32 col = 0; col < 4u; ++col) {
            out[row * 4u + col] = m.at(row, col);
        }
    }
    return true;
}

} // namespace

bool TaauUpscaler::resize(const UpscaleResolution& resolution) {
    if (!resolution.valid()) {
        return false;
    }
    if (resolution.render_width != m_resolution.render_width || resolution.render_height != m_resolution.render_height ||
        resolution.display_width != m_resolution.display_width ||
        resolution.display_height != m_resolution.display_height || m_history[0].empty()) {
        m_resolution = resolution;
        const usize dn = static_cast<usize>(resolution.display_width) * resolution.display_height;
        const usize rn = static_cast<usize>(resolution.render_width) * resolution.render_height;
        m_history[0].assign(dn, math::Vec4{});
        m_history[1].assign(dn, math::Vec4{});
        m_prevDepth.assign(rn, 0.f);
        m_prevMotion.assign(rn, math::Vec2{});
        m_current = 0;
        m_valid = false;
    }
    return true;
}

bool TaauUpscaler::upscale(const UpscaleInputs& in, math::Vec3* output, kernel::Backend backend) {
    m_stats = {};
    if (output == nullptr || validateUpscaleInputs(in) != UpscaleInputsError::None || !resize(in.resolution)) {
        return false;
    }
    if (in.reset_history) {
        m_valid = false;
    }
    const UpscaleResolution& r = in.resolution;
    const usize rn = in.renderPixelCount();
    const usize dn = in.displayPixelCount();
    const u32 next = m_current ^ 1u;

    taau_kernel::Params p{};
    p.color = constSpan(in.color, rn);
    p.depth = constSpan(in.depth, rn);
    p.motion = constSpan(in.motion, rn);
    p.reactive = constSpan(in.reactive, rn);
    p.transparency = constSpan(in.transparency_composition, rn);
    if (m_valid) {
        p.prev_depth = constSpan(m_prevDepth.data(), rn);
        p.prev_motion = constSpan(m_prevMotion.data(), rn);
    }
    p.history_in = constSpan(m_history[m_current].data(), dn);
    p.history_out = spanOf(m_history[next]);
    p.output = kernel::make_span(output, static_cast<u32>(dn));
    p.render_w = r.render_width;
    p.render_h = r.render_height;
    p.display_w = r.display_width;
    p.display_h = r.display_height;
    p.jitter_px = in.jitter_px;
    p.exposure = in.exposure;
    p.history_valid = m_valid ? 1u : 0u;
    p.settings = m_settings;
    if (in.camera.vertical_fov_rad > 0.f && in.camera.aspect > 0.f &&
        currentToPreviousView(in.camera, in.previous_camera, p.cur_to_prev_view)) {
        p.has_camera = 1u;
        p.tan_half_y = std::tan(0.5f * in.camera.vertical_fov_rad);
        p.tan_half_x = p.tan_half_y * in.camera.aspect;
    }
    if (!taau_kernel::params_valid(p)) {
        return false;
    }
    const kernel::LaunchResult result =
        kernel::launch(backend, taau_kernel::make_launch(r.display_width, r.display_height), taau_kernel::Kernel{}, p);
    if (!result.ok) {
        return false;
    }
    m_stats.resolved = true;
    m_stats.history_used = m_valid;
    m_stats.backend = result.backend;
    m_stats.duration_ns = result.duration_ns;

    std::copy(in.depth, in.depth + rn, m_prevDepth.begin());
    std::copy(in.motion, in.motion + rn, m_prevMotion.begin());
    m_current = next;
    m_valid = true;
    return true;
}

bool spatialUpscale(const UpscaleInputs& in, taau_kernel::SpatialFilter filter, math::Vec3* output,
                    kernel::Backend backend) {
    if (output == nullptr || !in.resolution.valid() || in.color == nullptr) {
        return false;
    }
    const UpscaleResolution& r = in.resolution;
    taau_kernel::SpatialParams p{};
    p.color = constSpan(in.color, in.renderPixelCount());
    p.output = kernel::make_span(output, static_cast<u32>(in.displayPixelCount()));
    p.render_w = r.render_width;
    p.render_h = r.render_height;
    p.display_w = r.display_width;
    p.display_h = r.display_height;
    p.jitter_px = in.jitter_px;
    p.filter = static_cast<u32>(filter);
    const kernel::KernelLaunch launch{taau_kernel::kSpatialName, kernel::extent2(r.display_width, r.display_height),
                                      taau_kernel::kWorkgroup};
    return kernel::launch(backend, launch, taau_kernel::SpatialKernel{}, p).ok;
}

} // namespace fuse::renderer
