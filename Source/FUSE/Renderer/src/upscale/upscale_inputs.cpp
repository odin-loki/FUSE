#include <fuse/renderer/upscale/upscale_inputs.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

f32 halton(u32 index, u32 base) {
    f32 f = 1.f;
    f32 r = 0.f;
    while (index > 0u) {
        f /= static_cast<f32>(base);
        r += f * static_cast<f32>(index % base);
        index /= base;
    }
    return r;
}

/// clip = P * V * p (column-major data[col * 4 + row]).
math::Vec4 toClip(const UpscaleCamera& camera, const math::Vec3& p, f32 w) {
    const math::Mat4 vp = camera.projection * camera.view;
    const auto& d = vp.data;
    return {d[0] * p.x + d[4] * p.y + d[8] * p.z + d[12] * w, d[1] * p.x + d[5] * p.y + d[9] * p.z + d[13] * w,
            d[2] * p.x + d[6] * p.y + d[10] * p.z + d[14] * w, d[3] * p.x + d[7] * p.y + d[11] * p.z + d[15] * w};
}

bool clipToUv(const math::Vec4& clip, math::Vec2& uv) {
    if (!(clip.w > 1e-6f)) {
        return false;
    }
    const f32 inv = 1.f / clip.w;
    uv = {0.5f + 0.5f * clip.x * inv, 0.5f - 0.5f * clip.y * inv};
    return true;
}

} // namespace

f32 upscaleRatioForMode(UpscaleQualityMode mode) {
    switch (mode) {
    case UpscaleQualityMode::NativeAA:
        return 1.f;
    case UpscaleQualityMode::UltraQuality:
        return 1.3f;
    case UpscaleQualityMode::Quality:
        return 1.5f;
    case UpscaleQualityMode::Balanced:
        return 1.7f;
    case UpscaleQualityMode::Performance:
        return 2.f;
    case UpscaleQualityMode::UltraPerformance:
        return 3.f;
    }
    return 1.f;
}

const char* upscaleQualityModeLabel(UpscaleQualityMode mode) {
    switch (mode) {
    case UpscaleQualityMode::NativeAA:
        return "NativeAA";
    case UpscaleQualityMode::UltraQuality:
        return "UltraQuality";
    case UpscaleQualityMode::Quality:
        return "Quality";
    case UpscaleQualityMode::Balanced:
        return "Balanced";
    case UpscaleQualityMode::Performance:
        return "Performance";
    case UpscaleQualityMode::UltraPerformance:
        return "UltraPerformance";
    }
    return "Unknown";
}

UpscaleResolution makeUpscaleResolution(u32 displayWidth, u32 displayHeight, f32 ratio) {
    const f32 r = std::isfinite(ratio) ? std::max(1.f, ratio) : 1.f;
    UpscaleResolution res{};
    res.display_width = displayWidth;
    res.display_height = displayHeight;
    res.render_width = std::max(1u, static_cast<u32>(std::lround(static_cast<f64>(displayWidth) / r)));
    res.render_height = std::max(1u, static_cast<u32>(std::lround(static_cast<f64>(displayHeight) / r)));
    res.render_width = std::min(res.render_width, std::max(1u, displayWidth));
    res.render_height = std::min(res.render_height, std::max(1u, displayHeight));
    return res;
}

UpscaleResolution makeUpscaleResolution(u32 displayWidth, u32 displayHeight, UpscaleQualityMode mode) {
    return makeUpscaleResolution(displayWidth, displayHeight, upscaleRatioForMode(mode));
}

f32 upscaleTextureMipBias(const UpscaleResolution& resolution, f32 offset) {
    if (resolution.render_width == 0u || resolution.display_width == 0u) {
        return 0.f;
    }
    return std::log2(static_cast<f32>(resolution.render_width) / static_cast<f32>(resolution.display_width)) + offset;
}

u32 upscaleJitterPhaseCount(const UpscaleResolution& resolution) {
    if (resolution.render_width == 0u) {
        return 8u;
    }
    const f32 ratio = static_cast<f32>(resolution.display_width) / static_cast<f32>(resolution.render_width);
    return std::max(1u, static_cast<u32>(std::ceil(8.f * ratio * ratio - 1e-4f)));
}

math::Vec2 upscaleJitterOffset(u32 frameIndex, u32 phaseCount) {
    const u32 n = std::max(1u, phaseCount);
    const u32 i = frameIndex % n + 1u;
    return {halton(i, 2u) - 0.5f, halton(i, 3u) - 0.5f};
}

math::Vec2 upscaleJitterNdc(const math::Vec2& jitterPx, u32 renderWidth, u32 renderHeight) {
    if (renderWidth == 0u || renderHeight == 0u) {
        return {};
    }
    // Render pixel (i, j) samples the unjittered scene at (i + 0.5 + jitter): every point must move by -jitter
    // pixels, i.e. clip.xy -= 2 jitter / size * clip.w in Vulkan clip space (NDC y down) — the same matrix as
    // temporal::jitter_view_proj.
    return {-2.f * jitterPx.x / static_cast<f32>(renderWidth), -2.f * jitterPx.y / static_cast<f32>(renderHeight)};
}

math::Mat4 jitterProjection(const math::Mat4& projection, const math::Vec2& jitterNdc) {
    math::Mat4 out = projection;
    for (u32 col = 0; col < 4u; ++col) {
        const f32 w = projection.data[col * 4u + 3u];
        out.data[col * 4u + 0u] += jitterNdc.x * w;
        out.data[col * 4u + 1u] += jitterNdc.y * w;
    }
    return out;
}

const char* upscaleInputsErrorLabel(UpscaleInputsError error) {
    switch (error) {
    case UpscaleInputsError::None:
        return "None";
    case UpscaleInputsError::InvalidResolution:
        return "InvalidResolution";
    case UpscaleInputsError::MissingColor:
        return "MissingColor";
    case UpscaleInputsError::MissingDepth:
        return "MissingDepth";
    case UpscaleInputsError::MissingMotion:
        return "MissingMotion";
    case UpscaleInputsError::InvalidJitter:
        return "InvalidJitter";
    case UpscaleInputsError::InvalidExposure:
        return "InvalidExposure";
    }
    return "Unknown";
}

UpscaleInputsError validateUpscaleInputs(const UpscaleInputs& in) {
    if (!in.resolution.valid()) {
        return UpscaleInputsError::InvalidResolution;
    }
    if (in.color == nullptr) {
        return UpscaleInputsError::MissingColor;
    }
    if (in.depth == nullptr) {
        return UpscaleInputsError::MissingDepth;
    }
    if (in.motion == nullptr) {
        return UpscaleInputsError::MissingMotion;
    }
    const auto jitterOk = [](f32 j) { return std::isfinite(j) && j >= -0.5f && j <= 0.5f; };
    if (!jitterOk(in.jitter_px.x) || !jitterOk(in.jitter_px.y) || in.jitter_phase_count == 0u) {
        return UpscaleInputsError::InvalidJitter;
    }
    if (!(in.exposure > 0.f) || !std::isfinite(in.exposure)) {
        return UpscaleInputsError::InvalidExposure;
    }
    return UpscaleInputsError::None;
}

bool upscaleProjectToUv(const UpscaleCamera& camera, const math::Vec3& worldPos, math::Vec2& outUv) {
    return clipToUv(toClip(camera, worldPos, 1.f), outUv);
}

bool upscaleStaticMotion(const UpscaleCamera& current, const UpscaleCamera& previous, const math::Vec3& worldPos,
                         math::Vec2& outMotion) {
    math::Vec2 cur{};
    math::Vec2 prev{};
    if (!upscaleProjectToUv(current, worldPos, cur) || !upscaleProjectToUv(previous, worldPos, prev)) {
        return false;
    }
    outMotion = cur - prev;
    return true;
}

bool upscaleSkyMotion(const UpscaleCamera& current, const UpscaleCamera& previous, const math::Vec3& worldDir,
                      math::Vec2& outMotion) {
    math::Vec2 cur{};
    math::Vec2 prev{};
    if (!clipToUv(toClip(current, worldDir, 0.f), cur) || !clipToUv(toClip(previous, worldDir, 0.f), prev)) {
        return false;
    }
    outMotion = cur - prev;
    return true;
}

void compositeUpscaledWithUi(const math::Vec3* scene, const math::Vec4* ui, u32 width, u32 height, math::Vec3* out) {
    if (scene == nullptr || out == nullptr) {
        return;
    }
    const usize count = static_cast<usize>(width) * height;
    for (usize i = 0; i < count; ++i) {
        const math::Vec3 s = scene[i];
        if (ui == nullptr) {
            out[i] = s;
            continue;
        }
        const math::Vec4 u = ui[i];
        const f32 k = 1.f - u.w;
        out[i] = {u.x + s.x * k, u.y + s.y * k, u.z + s.z * k};
    }
}

} // namespace fuse::renderer
