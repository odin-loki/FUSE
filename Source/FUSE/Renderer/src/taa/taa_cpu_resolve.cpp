#include <fuse/renderer/taa/taa_cpu_resolve.hpp>
#include <fuse/renderer/taa/taa_kernel_common.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

// The math lives in the device-safe taa_kernel_common.hpp (shared with the single-source TAAU kernel).
using taa_common::clamp_index;
using taa_common::lerp3;
using taa_common::max3;
using taa_common::min3;
using taa_common::saturate;

const math::Vec3& texel(const math::Vec3* image, u32 width, u32 height, i32 x, i32 y) {
    return taa_common::texel(image, width, height, x, y);
}

} // namespace

math::Vec3 taaRgbToYCoCg(const math::Vec3& rgb) {
    return taa_common::rgb_to_ycocg(rgb);
}

math::Vec3 taaYCoCgToRgb(const math::Vec3& c) {
    return taa_common::ycocg_to_rgb(c);
}

math::Vec3 taaClipToAabb(const math::Vec3& history, const math::Vec3& boxMin, const math::Vec3& boxMax) {
    return taa_common::clip_to_aabb(history, boxMin, boxMax);
}

math::Vec3 taaSampleBilinear(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py) {
    return taa_common::sample_bilinear(image, width, height, px, py);
}

math::Vec3 taaSampleCatmullRom(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py) {
    return taa_common::sample_catmull_rom(image, width, height, px, py);
}

bool TaaCpuResolver::resize(u32 width, u32 height) {
    if (width == 0u || height == 0u) {
        return false;
    }
    if (width != m_width || height != m_height) {
        m_width = width;
        m_height = height;
        const usize count = static_cast<usize>(width) * height;
        m_history.assign(count, math::Vec3{});
        m_historyVelocity.assign(count, math::Vec2{});
        m_frameVelocity.assign(count, math::Vec2{});
        m_historyDepth.assign(count, 0.f);
        m_valid = false;
    }
    return true;
}

void TaaCpuResolver::invalidate() {
    m_valid = false;
}

bool TaaCpuResolver::resolve(const TaaCpuFrameInputs& in, const TAAParams& rawParams, math::Vec3* output) {
    m_stats = {};
    if (in.current == nullptr || output == nullptr || in.width == 0u || in.height == 0u) {
        return false;
    }
    resize(in.width, in.height);
    const TAAParams params = clampTaaParams(rawParams);
    const u32 w = in.width;
    const u32 h = in.height;
    const i32 maxX = static_cast<i32>(w) - 1;
    const i32 maxY = static_cast<i32>(h) - 1;
    m_stats.first_frame = !m_valid;

    auto velocityAt = [&](i32 x, i32 y) -> math::Vec2 {
        if (in.velocity == nullptr) {
            return {};
        }
        return in.velocity[static_cast<u32>(clamp_index(y, maxY)) * w + static_cast<u32>(clamp_index(x, maxX))];
    };

    for (u32 y = 0u; y < h; ++y) {
        for (u32 x = 0u; x < w; ++x) {
            const u32 idx = y * w + x;
            const math::Vec3 current = in.current[idx];

            // Velocity (optionally from the closest-depth neighbour so edges reproject with the foreground).
            math::Vec2 velocity = velocityAt(static_cast<i32>(x), static_cast<i32>(y));
            if (m_options.dilate_velocity && in.velocity != nullptr && in.depth != nullptr) {
                f32 closest = in.depth[idx];
                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 nx = static_cast<i32>(x) + dx;
                        const i32 ny = static_cast<i32>(y) + dy;
                        if (nx < 0 || ny < 0 || nx > maxX || ny > maxY) {
                            continue;
                        }
                        const f32 d = in.depth[static_cast<u32>(ny) * w + static_cast<u32>(nx)];
                        if (d > 0.f && d < closest) {
                            closest = d;
                            velocity = velocityAt(nx, ny);
                        }
                    }
                }
            }
            m_frameVelocity[idx] = velocity;
            if (!m_valid) {
                output[idx] = current;
                continue;
            }

            const f32 prevX = static_cast<f32>(x) + 0.5f - velocity.x;
            const f32 prevY = static_cast<f32>(y) + 0.5f - velocity.y;
            if (prevX < 0.f || prevY < 0.f || prevX >= static_cast<f32>(w) || prevY >= static_cast<f32>(h)) {
                ++m_stats.offscreen_rejections;
                output[idx] = current;
                continue;
            }
            const u32 prevIdx = static_cast<u32>(prevY) * w + static_cast<u32>(prevX);

            // Depth-based disocclusion: the surface now visible was not the one stored in history.
            if (params.depth_rejection > 0.f && in.depth != nullptr && m_historyHasDepth) {
                const f32 d = in.depth[idx];
                const f32 dh = m_historyDepth[prevIdx];
                if (d > 0.f && dh > 0.f && std::fabs(d - dh) > params.depth_rejection * std::max(d, dh)) {
                    ++m_stats.depth_rejections;
                    output[idx] = current;
                    continue;
                }
            }

            math::Vec3 history = params.use_catmull_rom ? taaSampleCatmullRom(m_history.data(), w, h, prevX, prevY)
                                                        : taaSampleBilinear(m_history.data(), w, h, prevX, prevY);

            // Velocity disagreement: history was written by content moving differently from this pixel.
            const math::Vec2 historyVelocity = m_historyVelocity[prevIdx];
            const f32 disagreement = (velocity - historyVelocity).length();
            const f32 velocityWeight =
                saturate(params.velocity_rejection * disagreement / std::max(1e-4f, m_options.velocity_disagreement_px));
            if (velocityWeight > 0.5f) {
                ++m_stats.velocity_rejections;
            }

            // Neighbourhood statistics in YCoCg (mean +/- gamma * sigma, intersected with min/max).
            math::Vec3 nMin{1e30f, 1e30f, 1e30f};
            math::Vec3 nMax{-1e30f, -1e30f, -1e30f};
            math::Vec3 m1{};
            math::Vec3 m2{};
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const math::Vec3 c =
                        taaRgbToYCoCg(texel(in.current, w, h, static_cast<i32>(x) + dx, static_cast<i32>(y) + dy));
                    nMin = min3(nMin, c);
                    nMax = max3(nMax, c);
                    m1 = m1 + c;
                    m2 = m2 + math::Vec3{c.x * c.x, c.y * c.y, c.z * c.z};
                }
            }
            const math::Vec3 historyYcocg = taaRgbToYCoCg(history);
            math::Vec3 resolvedHistory = historyYcocg;
            if (m_options.neighborhood_clip) {
                const f32 motion = std::max(velocity.length(), disagreement);
                const f32 clipStrength =
                    std::max(saturate(m_options.static_clip_strength),
                             saturate(motion / std::max(1e-4f, m_options.clip_full_motion_px)));
                if (clipStrength > 0.f) {
                    const math::Vec3 mean = m1 * (1.f / 9.f);
                    const math::Vec3 var = m2 * (1.f / 9.f) - math::Vec3{mean.x * mean.x, mean.y * mean.y, mean.z * mean.z};
                    const math::Vec3 sigma{std::sqrt(std::max(0.f, var.x)), std::sqrt(std::max(0.f, var.y)),
                                           std::sqrt(std::max(0.f, var.z))};
                    const math::Vec3 boxMin = max3(nMin, mean - sigma * params.clamp_gamma);
                    const math::Vec3 boxMax = min3(nMax, mean + sigma * params.clamp_gamma);
                    const math::Vec3 clipped = taaClipToAabb(historyYcocg, min3(boxMin, boxMax), max3(boxMin, boxMax));
                    resolvedHistory = lerp3(historyYcocg, clipped, clipStrength);
                    if ((resolvedHistory - historyYcocg).length() > 1e-3f) {
                        ++m_stats.clipped_pixels;
                    }
                }
            }
            if (params.use_catmull_rom) {
                // Catmull-Rom can ring; never let it leave the neighbourhood+history hull.
                resolvedHistory = max3(min3(resolvedHistory, max3(nMax, historyYcocg)), min3(nMin, historyYcocg));
            }
            history = taaYCoCgToRgb(resolvedHistory);

            const f32 alpha = params.blend_factor + (1.f - params.blend_factor) * velocityWeight;
            output[idx] = lerp3(history, current, alpha);
        }
    }

    // Commit history for the next frame.
    std::copy(output, output + static_cast<usize>(w) * h, m_history.begin());
    for (u32 i = 0u; i < w * h; ++i) {
        m_historyVelocity[i] = m_frameVelocity[i];
        m_historyDepth[i] = in.depth != nullptr ? in.depth[i] : 0.f;
    }
    m_historyHasDepth = in.depth != nullptr;
    m_valid = true;
    m_stats.resolved = true;
    return true;
}

} // namespace fuse::renderer
