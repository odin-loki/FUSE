#include <fuse/renderer/postprocess/dof.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

f32 dof_aperture_diameter_mm(const DOFParams& params) {
    return params.focal_length / std::max(params.f_stop, 1e-4f);
}

f32 dof_coc_diameter_mm(f32 object_distance_m, const DOFParams& params) {
    const f32 f = params.focal_length;
    const f32 s1 = params.focal_distance * 1000.f;
    const f32 s2 = std::max(object_distance_m * 1000.f, 1e-3f);
    if (s1 <= f) {
        return 0.f;
    }
    const f32 aperture = dof_aperture_diameter_mm(params);
    return aperture * f * (s2 - s1) / (s2 * (s1 - f));
}

f32 dof_coc_radius_px(f32 object_distance_m, const DOFParams& params, u32 image_width_px) {
    const f32 cocMm = dof_coc_diameter_mm(object_distance_m, params);
    if (cocMm < 0.f && !params.near_blur) {
        return 0.f;
    }
    const f32 sensor = std::max(params.sensor_width, 1e-4f);
    return 0.5f * cocMm / sensor * static_cast<f32>(image_width_px);
}

f32 dof_linear_depth(f32 stored_depth, f32 near_plane, f32 far_plane, bool reversed_z) {
    const f32 d = std::max(stored_depth, 1e-8f);
    if (reversed_z) {
        if (far_plane <= 0.f) {
            return near_plane / d;
        }
        return far_plane * near_plane / (near_plane + stored_depth * (far_plane - near_plane));
    }
    return far_plane * near_plane / (far_plane - stored_depth * (far_plane - near_plane));
}

void dof_pass(const fuse::math::Vec3* color, const f32* linear_depth_m, u32 width, u32 height,
              const DOFParams& params, std::vector<fuse::math::Vec3>& out) {
    const size_t count = static_cast<size_t>(width) * height;
    out.assign(count, fuse::math::Vec3{});
    if (color == nullptr || count == 0u) {
        return;
    }
    if (linear_depth_m == nullptr || !params.enabled) {
        out.assign(color, color + count);
        return;
    }

    std::vector<fuse::math::Vec3> accum(count, fuse::math::Vec3{});
    std::vector<f32> weight(count, 0.f);
    const f32 maxRadius = std::max(params.max_coc_radius_px, 0.f);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const f32 radius = std::min(std::fabs(dof_coc_radius_px(linear_depth_m[index], params, width)), maxRadius);
            const s32 reach = static_cast<s32>(std::floor(radius));
            const f32 radiusSq = radius * radius;
            u32 area = 0u;
            for (s32 dy = -reach; dy <= reach; ++dy) {
                for (s32 dx = -reach; dx <= reach; ++dx) {
                    if (static_cast<f32>(dx * dx + dy * dy) <= radiusSq) {
                        ++area;
                    }
                }
            }
            const f32 w = 1.f / static_cast<f32>(std::max(area, 1u));
            for (s32 dy = -reach; dy <= reach; ++dy) {
                for (s32 dx = -reach; dx <= reach; ++dx) {
                    if (static_cast<f32>(dx * dx + dy * dy) > radiusSq) {
                        continue;
                    }
                    const s64 tx = static_cast<s64>(x) + dx;
                    const s64 ty = static_cast<s64>(y) + dy;
                    if (tx < 0 || ty < 0 || tx >= static_cast<s64>(width) || ty >= static_cast<s64>(height)) {
                        continue;
                    }
                    const size_t target = static_cast<size_t>(ty) * width + static_cast<size_t>(tx);
                    accum[target] = accum[target] + color[index] * w;
                    weight[target] += w;
                }
            }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        out[i] = weight[i] > 0.f ? accum[i] * (1.f / weight[i]) : color[i];
    }
}

} // namespace fuse::renderer
