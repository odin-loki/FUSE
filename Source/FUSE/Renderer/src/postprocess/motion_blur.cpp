#include <fuse/renderer/postprocess/motion_blur.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

namespace {

using fuse::math::Vec2;
using fuse::math::Vec3;

constexpr f32 kMinHalfBlurPx = 0.5f;

f32 cone(f32 distance, f32 halfLength) {
    if (halfLength <= 1e-6f) {
        return 0.f;
    }
    return std::clamp(1.f - distance / halfLength, 0.f, 1.f);
}

f32 smoothstep(f32 edge0, f32 edge1, f32 x) {
    if (edge1 <= edge0) {
        return x < edge0 ? 0.f : 1.f;
    }
    const f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

f32 cylinder(f32 distance, f32 halfLength) {
    if (halfLength <= 1e-6f) {
        return 0.f;
    }
    return 1.f - smoothstep(0.95f * halfLength, 1.05f * halfLength, distance);
}

// 1 when `a` is in front of (or level with) `b`, fading to 0 once `a` is `extent` behind `b`.
f32 inFront(f32 a, f32 b, f32 extent) {
    return std::clamp(1.f - (a - b) / std::max(extent, 1e-6f), 0.f, 1.f);
}

Vec2 halfBlur(const Vec2& velocity, const MotionBlurParams& params) {
    return motion_blur_vector(velocity, params) * 0.5f;
}

} // namespace

fuse::math::Vec2 motion_blur_vector(const fuse::math::Vec2& velocity_px, const MotionBlurParams& params) {
    const f32 shutter = std::clamp(params.shutter_angle, 0.f, 360.f) / 360.f;
    Vec2 blur = velocity_px * shutter;
    const f32 length = blur.length();
    const f32 maxLength = std::max(params.max_blur_px, 0.f);
    if (length > maxLength && length > 0.f) {
        blur = blur * (maxLength / length);
    }
    return blur;
}

u32 motion_blur_tile_size(const MotionBlurParams& params) {
    return std::max(1u, static_cast<u32>(std::ceil(std::max(params.max_blur_px, 0.f) * 0.5f)));
}

void motion_blur_neighbor_max(const fuse::math::Vec2* velocity_px, u32 width, u32 height,
                              const MotionBlurParams& params, std::vector<fuse::math::Vec2>& out_tiles,
                              u32& tiles_x, u32& tiles_y) {
    const u32 tile = motion_blur_tile_size(params);
    tiles_x = (width + tile - 1u) / tile;
    tiles_y = (height + tile - 1u) / tile;
    std::vector<Vec2> tileMax(static_cast<size_t>(tiles_x) * tiles_y, Vec2{});
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const Vec2 h = halfBlur(velocity_px[static_cast<size_t>(y) * width + x], params);
            Vec2& slot = tileMax[static_cast<size_t>(y / tile) * tiles_x + x / tile];
            if (h.dot(h) > slot.dot(slot)) {
                slot = h;
            }
        }
    }
    out_tiles.assign(tileMax.size(), Vec2{});
    for (u32 ty = 0; ty < tiles_y; ++ty) {
        for (u32 tx = 0; tx < tiles_x; ++tx) {
            Vec2 best{};
            for (s32 oy = -1; oy <= 1; ++oy) {
                for (s32 ox = -1; ox <= 1; ++ox) {
                    const s64 nx = static_cast<s64>(tx) + ox;
                    const s64 ny = static_cast<s64>(ty) + oy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<s64>(tiles_x) || ny >= static_cast<s64>(tiles_y)) {
                        continue;
                    }
                    const Vec2& candidate = tileMax[static_cast<size_t>(ny) * tiles_x + static_cast<size_t>(nx)];
                    if (candidate.dot(candidate) > best.dot(best)) {
                        best = candidate;
                    }
                }
            }
            out_tiles[static_cast<size_t>(ty) * tiles_x + tx] = best;
        }
    }
}

void motion_blur_pass(const fuse::math::Vec3* color, const fuse::math::Vec2* velocity_px, const f32* linear_depth_m,
                      u32 width, u32 height, const MotionBlurParams& params, std::vector<fuse::math::Vec3>& out) {
    const size_t count = static_cast<size_t>(width) * height;
    out.assign(count, Vec3{});
    if (color == nullptr || count == 0u) {
        return;
    }
    out.assign(color, color + count);
    if (velocity_px == nullptr || !params.enabled || params.max_samples == 0u) {
        return;
    }

    std::vector<Vec2> neighborMax;
    u32 tilesX = 0;
    u32 tilesY = 0;
    motion_blur_neighbor_max(velocity_px, width, height, params, neighborMax, tilesX, tilesY);
    const u32 tile = motion_blur_tile_size(params);
    const u32 samples = params.max_samples;

    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const Vec2 vn = neighborMax[static_cast<size_t>(y / tile) * tilesX + x / tile];
            if (vn.length() < kMinHalfBlurPx) {
                continue;
            }

            const Vec2 vx = halfBlur(velocity_px[index], params);
            const f32 vxLength = vx.length();
            const f32 zx = linear_depth_m != nullptr ? linear_depth_m[index] : 0.f;

            f32 totalWeight = 1.f / std::max(vxLength, kMinHalfBlurPx);
            Vec3 sum = color[index] * totalWeight;
            for (u32 i = 0; i < samples; ++i) {
                const f32 t = -1.f + 2.f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(samples);
                const s64 sx = static_cast<s64>(std::lround(static_cast<f32>(x) + vn.x * t));
                const s64 sy = static_cast<s64>(std::lround(static_cast<f32>(y) + vn.y * t));
                if (sx < 0 || sy < 0 || sx >= static_cast<s64>(width) || sy >= static_cast<s64>(height)) {
                    continue;
                }
                if (sx == static_cast<s64>(x) && sy == static_cast<s64>(y)) {
                    continue;
                }
                const size_t sample = static_cast<size_t>(sy) * width + static_cast<size_t>(sx);
                const f32 dx = static_cast<f32>(sx) - static_cast<f32>(x);
                const f32 dy = static_cast<f32>(sy) - static_cast<f32>(y);
                const f32 distance = std::sqrt(dx * dx + dy * dy);
                const f32 vyLength = halfBlur(velocity_px[sample], params).length();
                const f32 zy = linear_depth_m != nullptr ? linear_depth_m[sample] : 0.f;

                const f32 foreground = inFront(zy, zx, params.soft_depth_extent);
                const f32 background = inFront(zx, zy, params.soft_depth_extent);
                const f32 weight = foreground * cone(distance, vyLength) + background * cone(distance, vxLength) +
                                   cylinder(distance, vyLength) * cylinder(distance, vxLength) * 2.f;
                sum = sum + color[sample] * weight;
                totalWeight += weight;
            }
            out[index] = sum * (1.f / totalWeight);
        }
    }
}

} // namespace fuse::renderer
