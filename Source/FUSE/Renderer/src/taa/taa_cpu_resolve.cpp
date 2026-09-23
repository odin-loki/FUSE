#include <fuse/renderer/taa/taa_cpu_resolve.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

f32 saturate(f32 v) {
    return std::max(0.f, std::min(1.f, v));
}

math::Vec3 lerp3(const math::Vec3& a, const math::Vec3& b, f32 t) {
    return a + (b - a) * t;
}

math::Vec3 min3(const math::Vec3& a, const math::Vec3& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

math::Vec3 max3(const math::Vec3& a, const math::Vec3& b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

i32 clampIndex(i32 v, i32 hi) {
    return v < 0 ? 0 : (v > hi ? hi : v);
}

const math::Vec3& texel(const math::Vec3* image, u32 width, u32 height, i32 x, i32 y) {
    const i32 cx = clampIndex(x, static_cast<i32>(width) - 1);
    const i32 cy = clampIndex(y, static_cast<i32>(height) - 1);
    return image[static_cast<u32>(cy) * width + static_cast<u32>(cx)];
}

void catmullRomWeights(f32 t, f32 w[4]) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    w[0] = 0.5f * (-t3 + 2.f * t2 - t);
    w[1] = 0.5f * (3.f * t3 - 5.f * t2 + 2.f);
    w[2] = 0.5f * (-3.f * t3 + 4.f * t2 + t);
    w[3] = 0.5f * (t3 - t2);
}

} // namespace

math::Vec3 taaRgbToYCoCg(const math::Vec3& rgb) {
    return {0.25f * rgb.x + 0.5f * rgb.y + 0.25f * rgb.z, 0.5f * rgb.x - 0.5f * rgb.z,
            -0.25f * rgb.x + 0.5f * rgb.y - 0.25f * rgb.z};
}

math::Vec3 taaYCoCgToRgb(const math::Vec3& c) {
    const f32 tmp = c.x - c.z;
    return {tmp + c.y, c.x + c.z, tmp - c.y};
}

math::Vec3 taaClipToAabb(const math::Vec3& history, const math::Vec3& boxMin, const math::Vec3& boxMax) {
    const math::Vec3 center = (boxMin + boxMax) * 0.5f;
    const math::Vec3 extent = (boxMax - boxMin) * 0.5f;
    const math::Vec3 offset = history - center;
    const f32 eps = 1e-6f;
    const f32 ux = std::fabs(offset.x) / std::max(extent.x, eps);
    const f32 uy = std::fabs(offset.y) / std::max(extent.y, eps);
    const f32 uz = std::fabs(offset.z) / std::max(extent.z, eps);
    const f32 maxUnit = std::max(ux, std::max(uy, uz));
    if (maxUnit > 1.f) {
        return center + offset * (1.f / maxUnit);
    }
    return history;
}

math::Vec3 taaSampleBilinear(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py) {
    const f32 fx = px - 0.5f;
    const f32 fy = py - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx));
    const i32 y0 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 ty = fy - static_cast<f32>(y0);
    const math::Vec3 top = lerp3(texel(image, width, height, x0, y0), texel(image, width, height, x0 + 1, y0), tx);
    const math::Vec3 bottom =
        lerp3(texel(image, width, height, x0, y0 + 1), texel(image, width, height, x0 + 1, y0 + 1), tx);
    return lerp3(top, bottom, ty);
}

math::Vec3 taaSampleCatmullRom(const math::Vec3* image, u32 width, u32 height, f32 px, f32 py) {
    const f32 fx = px - 0.5f;
    const f32 fy = py - 0.5f;
    const i32 x1 = static_cast<i32>(std::floor(fx));
    const i32 y1 = static_cast<i32>(std::floor(fy));
    f32 wx[4];
    f32 wy[4];
    catmullRomWeights(fx - static_cast<f32>(x1), wx);
    catmullRomWeights(fy - static_cast<f32>(y1), wy);
    math::Vec3 sum{};
    for (i32 j = 0; j < 4; ++j) {
        math::Vec3 row{};
        for (i32 i = 0; i < 4; ++i) {
            row = row + texel(image, width, height, x1 - 1 + i, y1 - 1 + j) * wx[i];
        }
        sum = sum + row * wy[j];
    }
    return sum;
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
        return in.velocity[static_cast<u32>(clampIndex(y, maxY)) * w + static_cast<u32>(clampIndex(x, maxX))];
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
