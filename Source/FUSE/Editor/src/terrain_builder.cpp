#include <fuse/editor/terrain_builder.hpp>

#include <fuse/terrain/chunk_mesh.hpp>
#include <fuse/terrain/terrain_noise.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::editor {

ecs::Terrain sanitizedTerrain(const ecs::Terrain& in) {
    ecs::Terrain t = in;
    t.cells = std::clamp(t.cells, 2u, 256u);
    if (!(t.size > 0.01f) || !std::isfinite(t.size)) t.size = 64.f;
    if (!std::isfinite(t.height)) t.height = 0.f;
    t.height = std::clamp(t.height, 0.f, 10000.f);
    t.octaves = std::clamp(t.octaves, 1u, terrain::kMaxNoiseOctaves);
    t.stamp_count = std::min(t.stamp_count, ecs::Terrain::kMaxStamps);
    return t;
}

void buildTerrainHeightfield(const ecs::Terrain& in, terrain::Heightfield& out) {
    const ecs::Terrain t = sanitizedTerrain(in);
    terrain::TerrainDesc desc{};
    desc.resolution = t.cells + 1u;
    desc.world_size = t.size;
    desc.max_height = t.height;
    desc.has_svo_caves = false;
    out.init(desc);
    std::vector<f32>& h = out.mutable_heights();
    if (t.height > 0.f) {
        terrain::TerrainNoiseParams params = terrain::make_terrain_noise_params(desc);
        params.octaves = t.octaves;
        terrain::generate_terrain_heights(t.seed, params, desc.resolution, t.height, h.data());
        // Centre the noise band on y = 0 so the entity's height is the terrain's mid level.
        for (f32& v : h) v -= 0.5f * t.height;
    }
    const f32 step = t.size / static_cast<f32>(t.cells);
    const f32 half = 0.5f * t.size;
    for (u32 s = 0; s < t.stamp_count; ++s) {
        const ecs::TerrainStamp& st = t.stamps[s];
        if (!(st.radius > 0.f)) continue;
        const f32 r2 = st.radius * st.radius;
        const s32 x0 = std::max(0, static_cast<s32>(std::floor((st.x + half - st.radius) / step)));
        const s32 x1 = std::min(static_cast<s32>(t.cells), static_cast<s32>(std::ceil((st.x + half + st.radius) / step)));
        const s32 z0 = std::max(0, static_cast<s32>(std::floor((st.z + half - st.radius) / step)));
        const s32 z1 = std::min(static_cast<s32>(t.cells), static_cast<s32>(std::ceil((st.z + half + st.radius) / step)));
        for (s32 iz = z0; iz <= z1; ++iz) {
            for (s32 ix = x0; ix <= x1; ++ix) {
                const f32 dx = static_cast<f32>(ix) * step - half - st.x;
                const f32 dz = static_cast<f32>(iz) * step - half - st.z;
                const f32 d2 = dx * dx + dz * dz;
                if (d2 >= r2) continue;
                const f32 k = 1.f - d2 / r2;
                const f32 w = k * k; // smooth falloff, 1 at the centre, 0 with zero slope at the rim
                f32& v = h[static_cast<usize>(iz) * desc.resolution + static_cast<usize>(ix)];
                switch (st.mode) {
                case ecs::TerrainStamp::Lower: v -= st.amount * w; break;
                case ecs::TerrainStamp::Flatten: v += (st.amount - v) * w; break;
                default: v += st.amount * w; break;
                }
            }
        }
    }
}

bool buildTerrainGeometry(const ecs::Terrain& in, TerrainGeometry& out) {
    const ecs::Terrain t = sanitizedTerrain(in);
    terrain::Heightfield field;
    buildTerrainHeightfield(t, field);
    terrain::ChunkMeshDesc desc{};
    desc.chunk_coord = {0, 0};
    desc.chunk_size = t.size;
    desc.chunk_resolution = t.cells;
    terrain::ChunkMesh mesh;
    terrain::build_chunk_mesh(field, desc, mesh);
    if (mesh.empty()) return false;
    const f32 half = 0.5f * t.size;
    out.positions.resize(mesh.positions.size() * 3u);
    out.normals.resize(mesh.positions.size() * 3u);
    out.uvs.resize(mesh.positions.size() * 2u);
    out.lo[0] = out.lo[2] = -half;
    out.hi[0] = out.hi[2] = half;
    out.lo[1] = 1e30f;
    out.hi[1] = -1e30f;
    for (usize i = 0; i < mesh.positions.size(); ++i) {
        const auto& p = mesh.positions[i];
        out.positions[i * 3u + 0u] = p.x - half;
        out.positions[i * 3u + 1u] = p.y;
        out.positions[i * 3u + 2u] = p.z - half;
        out.normals[i * 3u + 0u] = mesh.normals[i].x;
        out.normals[i * 3u + 1u] = mesh.normals[i].y;
        out.normals[i * 3u + 2u] = mesh.normals[i].z;
        out.uvs[i * 2u + 0u] = p.x / t.size;
        out.uvs[i * 2u + 1u] = p.z / t.size;
        out.lo[1] = std::min(out.lo[1], p.y);
        out.hi[1] = std::max(out.hi[1], p.y);
    }
    if (out.hi[1] - out.lo[1] < 1e-3f) {
        out.lo[1] -= 0.01f;
        out.hi[1] += 0.01f;
    }
    out.indices = std::move(mesh.indices);
    return true;
}

f32 terrainHeightAt(const terrain::Heightfield& field, const ecs::Terrain& t, f32 x, f32 z) {
    const f32 half = 0.5f * sanitizedTerrain(t).size;
    return field.sample_height(x + half, z + half);
}

bool addTerrainStamp(ecs::Terrain& t, const ecs::TerrainStamp& stamp) {
    if (t.stamp_count >= ecs::Terrain::kMaxStamps) return false;
    t.stamps[t.stamp_count++] = stamp;
    return true;
}

bool raycastTerrain(const terrain::Heightfield& field, const ecs::Terrain& in, const f32 origin[3], const f32 dir[3],
                    f32 hit[3]) {
    const ecs::Terrain t = sanitizedTerrain(in);
    const f32 half = 0.5f * t.size;
    // Clip the ray to the terrain's XZ square.
    f32 tmin = 0.f, tmax = 1e6f;
    for (int a : {0, 2}) {
        if (std::fabs(dir[a]) < 1e-8f) {
            if (origin[a] < -half || origin[a] > half) return false;
            continue;
        }
        f32 t0 = (-half - origin[a]) / dir[a], t1 = (half - origin[a]) / dir[a];
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
    }
    if (tmin > tmax) return false;
    const auto above = [&](f32 s) {
        const f32 x = origin[0] + dir[0] * s, y = origin[1] + dir[1] * s, z = origin[2] + dir[2] * s;
        return y - terrainHeightAt(field, t, x, z);
    };
    const f32 step = std::max(t.size / static_cast<f32>(t.cells) * 0.25f, 1e-3f);
    f32 prev = tmin;
    f32 prevD = above(prev);
    if (prevD < 0.f) return false; // starts below the surface
    for (f32 s = tmin + step; s <= tmax + step; s += step) {
        const f32 cur = std::min(s, tmax);
        const f32 d = above(cur);
        if (d <= 0.f) {
            f32 a = prev, b = cur;
            for (int i = 0; i < 24; ++i) {
                const f32 m = 0.5f * (a + b);
                (above(m) > 0.f ? a : b) = m;
            }
            const f32 h = 0.5f * (a + b);
            for (int k = 0; k < 3; ++k) hit[k] = origin[k] + dir[k] * h;
            return true;
        }
        prev = cur;
        prevD = d;
        if (cur >= tmax) break;
    }
    return false;
}

u64 terrainHash(const ecs::Terrain& t) {
    u64 h = 1469598103934665603ull;
    const auto* b = reinterpret_cast<const unsigned char*>(&t);
    for (usize i = 0; i < sizeof(ecs::Terrain); ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace fuse::editor
