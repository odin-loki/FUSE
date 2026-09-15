#include <fuse/terrain/terrain.hpp>

#include <cmath>

namespace fuse::terrain {

namespace {

f32 hash_noise(u64 seed, u32 x, u32 z) {
    u64 h = seed;
    h ^= static_cast<u64>(x) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<u64>(z) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    const f32 unit = static_cast<f32>(h & 0xFFFFFFull) / static_cast<f32>(0xFFFFFFull);
    return unit * 2.f - 1.f;
}

} // namespace

void Terrain::init(const TerrainDesc& desc) {
    destroy();
    m_desc = desc;
    m_heightfield.init(desc);
    m_chunk_grid.init(desc);
    m_initialized = true;
}

void Terrain::destroy() {
    m_chunk_grid.destroy();
    m_heightfield.destroy();
    m_initialized = false;
}

void Terrain::generate(u64 seed) {
    if (!m_initialized) {
        return;
    }

    const u32 resolution = m_heightfield.resolution();
    for (u32 z = 0; z < resolution; ++z) {
        for (u32 x = 0; x < resolution; ++x) {
            const f32 noise = hash_noise(seed, x, z);
            const f32 height = (noise * 0.5f + 0.5f) * m_desc.max_height;
            m_heightfield.set_height(x, z, height);
        }
    }
}

void Terrain::deform(vec3 world_pos, f32 radius, f32 amount) {
    if (!m_initialized || radius <= 0.f) {
        return;
    }

    const f32 meters_per_texel = m_desc.world_size / static_cast<f32>(std::max(m_desc.resolution - 1, 1u));
    const u32 texel_radius = static_cast<u32>(std::ceil(radius / meters_per_texel));

    const f32 u = world_pos.x / meters_per_texel;
    const f32 v = world_pos.z / meters_per_texel;
    const s32 centre_x = static_cast<s32>(std::round(u));
    const s32 centre_z = static_cast<s32>(std::round(v));

    for (s32 dz = -static_cast<s32>(texel_radius); dz <= static_cast<s32>(texel_radius); ++dz) {
        for (s32 dx = -static_cast<s32>(texel_radius); dx <= static_cast<s32>(texel_radius); ++dx) {
            const s32 x = centre_x + dx;
            const s32 z = centre_z + dz;
            if (x < 0 || z < 0) {
                continue;
            }
            const u32 ux = static_cast<u32>(x);
            const u32 uz = static_cast<u32>(z);
            if (ux >= m_desc.resolution || uz >= m_desc.resolution) {
                continue;
            }

            const f32 dist = std::sqrt(static_cast<f32>(dx * dx + dz * dz)) * meters_per_texel;
            if (dist > radius) {
                continue;
            }

            const f32 falloff = 1.f - (dist / radius);
            const f32 current = m_heightfield.get_height_texel(ux, uz);
            m_heightfield.set_height(ux, uz, current + amount * falloff);
        }
    }
}

f32 Terrain::get_height(f32 world_x, f32 world_z) const {
    return m_heightfield.sample_height(world_x, world_z);
}

vec3 Terrain::get_normal(f32 world_x, f32 world_z) const {
    return m_heightfield.sample_normal(world_x, world_z);
}

bool Terrain::ray_cast(vec3 origin, vec3 direction, f32 max_distance, vec3& hit, vec3& normal, f32& distance) const {
    const HeightfieldRayHit result = raycast_heightfield(m_heightfield, origin, direction, max_distance);
    if (!result.hit) {
        return false;
    }

    hit = result.position;
    normal = result.normal;
    distance = result.distance;
    return true;
}

void Terrain::update_lod(vec3 camera_pos, f32 dt) {
    m_chunk_grid.update_lod(camera_pos, dt);
}

void Terrain::get_visible_chunks(std::vector<const TerrainChunk*>& out) const {
    m_chunk_grid.collect_visible_chunks(out);
}

} // namespace fuse::terrain
