#include <fuse/terrain/terrain_caves.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::terrain {

void TerrainCaves::init(const TerrainDesc& desc) {
    destroy();
    const u32 depth = std::clamp(desc.svo_depth, 1u, 12u);
    m_origin = {0.f, desc.max_height - desc.world_size, 0.f};
    m_voxel_size = desc.world_size / static_cast<f32>(1u << depth);

    fuse::scene::SVODesc svo_desc{};
    svo_desc.origin = {m_origin.x, m_origin.y, m_origin.z};
    svo_desc.rootSize = desc.world_size;
    svo_desc.maxDepth = depth;
    svo_desc.storeSdf = false;
    m_svo.init(svo_desc);
}

void TerrainCaves::destroy() {
    m_svo.destroy();
    m_spheres.clear();
}

bool TerrainCaves::to_voxel(vec3 point, fuse::scene::ivec3& out) const {
    if (!m_svo.isInitialized() || m_voxel_size <= 0.f) {
        return false;
    }
    const f32 fx = std::floor((point.x - m_origin.x) / m_voxel_size);
    const f32 fy = std::floor((point.y - m_origin.y) / m_voxel_size);
    const f32 fz = std::floor((point.z - m_origin.z) / m_voxel_size);
    const f32 limit = static_cast<f32>(std::numeric_limits<s32>::max() / 2);
    if (std::fabs(fx) > limit || std::fabs(fy) > limit || std::fabs(fz) > limit) {
        return false;
    }
    out = {static_cast<s32>(fx), static_cast<s32>(fy), static_cast<s32>(fz)};
    return true;
}

void TerrainCaves::carve_sphere(vec3 center, f32 radius) {
    if (!m_svo.isInitialized() || radius <= 0.f) {
        return;
    }
    fuse::scene::ivec3 lo{};
    fuse::scene::ivec3 hi{};
    if (!to_voxel(center - vec3{radius, radius, radius}, lo) || !to_voxel(center + vec3{radius, radius, radius}, hi)) {
        return;
    }
    const f32 r2 = radius * radius;
    for (s32 z = lo.z; z <= hi.z; ++z) {
        for (s32 y = lo.y; y <= hi.y; ++y) {
            for (s32 x = lo.x; x <= hi.x; ++x) {
                const vec3 c{m_origin.x + (static_cast<f32>(x) + 0.5f) * m_voxel_size,
                             m_origin.y + (static_cast<f32>(y) + 0.5f) * m_voxel_size,
                             m_origin.z + (static_cast<f32>(z) + 0.5f) * m_voxel_size};
                const vec3 d = c - center;
                if (d.dot(d) <= r2) {
                    m_svo.set({x, y, z}, kCaveAirMaterial);
                }
            }
        }
    }
    m_spheres.push_back({center, radius});
}

bool TerrainCaves::is_cave(vec3 point) const {
    if (m_spheres.empty()) {
        return false;
    }
    fuse::scene::ivec3 voxel{};
    return to_voxel(point, voxel) && m_svo.get(voxel) == kCaveAirMaterial;
}

vec3 TerrainCaves::wall_normal(vec3 point) const {
    const Sphere* best = nullptr;
    f32 best_gap = std::numeric_limits<f32>::max();
    for (const Sphere& sphere : m_spheres) {
        const f32 gap = std::fabs((point - sphere.center).length() - sphere.radius);
        if (gap < best_gap) {
            best_gap = gap;
            best = &sphere;
        }
    }
    if (best == nullptr) {
        return {0.f, 1.f, 0.f};
    }
    const vec3 inward = (best->center - point).normalized();
    return inward.length() > 0.f ? inward : vec3{0.f, 1.f, 0.f};
}

} // namespace fuse::terrain
