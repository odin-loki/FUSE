#include <fuse/terrain/terrain.hpp>

#include <fuse/terrain/terrain_noise.hpp>

#include <algorithm>

#include <cmath>

namespace fuse::terrain {


void Terrain::init(const TerrainDesc& desc) {
    destroy();
    m_desc = desc;
    m_heightfield.init(desc);
    m_chunk_grid.init(desc);
    if (desc.has_svo_caves) {
        m_caves.init(desc);
    }
    m_meshes.assign(m_chunk_grid.chunk_count(), ChunkMesh{});
    m_height_dirty.assign(m_chunk_grid.chunk_count(), 0u);
    m_mesh_builds = 0;
    m_initialized = true;
}

void Terrain::destroy() {
    m_chunk_grid.destroy();
    m_heightfield.destroy();
    m_caves.destroy();
    m_meshes.clear();
    m_height_dirty.clear();
    m_initialized = false;
}

void Terrain::generate(u64 seed) {
    if (!m_initialized) {
        return;
    }

    const u32 resolution = m_heightfield.resolution();
    std::vector<f32>& heights = m_heightfield.mutable_heights();
    if (heights.size() != static_cast<usize>(resolution) * resolution) {
        return;
    }
    generate_terrain_heights(seed, make_terrain_noise_params(m_desc), resolution, m_desc.max_height,
                             heights.data());
    std::fill(m_height_dirty.begin(), m_height_dirty.end(), static_cast<u8>(1u));
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

    // Bilinear sampling spreads a texel edit one texel further in each direction.
    const f32 reach = static_cast<f32>(texel_radius + 1u) * meters_per_texel;
    mark_height_dirty_(world_pos.x - reach, world_pos.z - reach, world_pos.x + reach, world_pos.z + reach);
}

void Terrain::mark_height_dirty_(f32 min_x, f32 min_z, f32 max_x, f32 max_z) {
    for (u32 index = 0; index < m_chunk_grid.chunk_count(); ++index) {
        const AABB& bounds = m_chunk_grid.chunk(index).world_bounds;
        if (bounds.max.x < min_x || bounds.min.x > max_x || bounds.max.z < min_z || bounds.min.z > max_z) {
            continue;
        }
        m_height_dirty[index] = 1u;
    }
}

u32 Terrain::height_dirty_chunk_count() const {
    u32 count = 0;
    for (u32 index = 0; index < m_height_dirty.size() && index < m_chunk_grid.chunk_count(); ++index) {
        if (m_height_dirty[index] != 0u && is_resident_state(m_chunk_grid.chunk(index).residency)) {
            ++count;
        }
    }
    return count;
}

const ChunkMesh* Terrain::chunk_mesh(u32 chunk_index) const {
    if (chunk_index >= m_meshes.size() || m_meshes[chunk_index].empty()) {
        return nullptr;
    }
    return &m_meshes[chunk_index];
}

void Terrain::refresh_chunk_meshes_() {
    const u32 count = m_chunk_grid.chunk_count();
    if (m_meshes.size() != count) {
        m_meshes.assign(count, ChunkMesh{});
        m_height_dirty.assign(count, 1u);
    }

    for (u32 index = 0; index < count; ++index) {
        const TerrainChunk& chunk = m_chunk_grid.chunk(index);
        ChunkMesh& mesh = m_meshes[index];
        if (!is_resident_state(chunk.residency)) {
            if (!mesh.empty()) {
                mesh = ChunkMesh{};
            }
            continue;
        }

        ChunkMeshDesc desc{};
        desc.chunk_coord = chunk.chunk_coord;
        desc.chunk_size = m_chunk_grid.base_chunk_stride();
        desc.chunk_resolution = m_desc.chunk_resolution;
        desc.lod = chunk.lod;
        m_chunk_grid.neighbor_lods(index, desc.neighbor_lod);

        bool stale = mesh.empty() || m_height_dirty[index] != 0u || mesh.lod != desc.lod;
        for (u32 e = 0; e < kChunkEdgeCount && !stale; ++e) {
            stale = mesh.neighbor_lod[e] != desc.neighbor_lod[e];
        }
        if (!stale) {
            continue;
        }

        build_chunk_mesh(m_heightfield, desc, mesh);
        m_height_dirty[index] = 0u;
        ++m_mesh_builds;
    }
}

f32 Terrain::get_height(f32 world_x, f32 world_z) const {
    return m_heightfield.sample_height(world_x, world_z);
}

vec3 Terrain::get_normal(f32 world_x, f32 world_z) const {
    return m_heightfield.sample_normal(world_x, world_z);
}

void Terrain::carve_cave(vec3 center, f32 radius) {
    if (!m_initialized || !m_caves.is_initialized()) {
        return;
    }
    m_caves.carve_sphere(center, radius);
}

bool Terrain::is_solid(vec3 world_pos) const {
    if (!m_initialized || world_pos.y > m_heightfield.sample_height(world_pos.x, world_pos.z)) {
        return false;
    }
    return !m_caves.is_cave(world_pos);
}

bool Terrain::ray_cast(vec3 origin, vec3 direction, f32 max_distance, vec3& hit, vec3& normal, f32& distance) const {
    if (m_caves.empty()) {
        const HeightfieldRayHit result = raycast_heightfield(m_heightfield, origin, direction, max_distance);
        if (!result.hit) {
            return false;
        }
        hit = result.position;
        normal = result.normal;
        distance = result.distance;
        return true;
    }

    // Hybrid march: occupancy = below heightfield AND not cave air. Step finer than both the texel
    // and the voxel, then bisect the air->solid bracket.
    const vec3 dir = direction.normalized();
    if (!m_initialized || dir.length() <= 0.f || max_distance <= 0.f) {
        return false;
    }
    const f32 step = std::max(std::min(m_heightfield.meters_per_texel(), m_caves.voxel_size()) * 0.25f, 1e-3f);
    f32 prev_t = 0.f;
    bool prev_solid = is_solid(origin);
    for (f32 t = step;; t += step) {
        const f32 clamped = std::min(t, max_distance);
        const bool solid = is_solid(origin + dir * clamped);
        if (!prev_solid && solid) {
            f32 lo = prev_t;
            f32 hi = clamped;
            for (int i = 0; i < 24; ++i) {
                const f32 mid = 0.5f * (lo + hi);
                (is_solid(origin + dir * mid) ? hi : lo) = mid;
            }
            hit = origin + dir * hi;
            distance = hi;
            // Air just before the hit tells which surface was struck.
            const vec3 before = origin + dir * lo;
            const bool cave_wall = m_caves.is_cave(before) || m_caves.is_cave(hit - dir * m_caves.voxel_size());
            normal = cave_wall && before.y <= m_heightfield.sample_height(before.x, before.z)
                         ? m_caves.wall_normal(hit)
                         : m_heightfield.sample_normal(hit.x, hit.z);
            return true;
        }
        prev_t = clamped;
        prev_solid = solid;
        if (clamped >= max_distance) {
            return false;
        }
    }
}

void Terrain::update_lod(vec3 camera_pos, f32 dt) {
    if (!m_initialized) {
        return;
    }
    m_chunk_grid.update_lod(camera_pos, dt);
    refresh_chunk_meshes_();
}

void Terrain::get_visible_chunks(std::vector<const TerrainChunk*>& out) const {
    m_chunk_grid.collect_visible_chunks(out);
}

} // namespace fuse::terrain
