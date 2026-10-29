#pragma once

// GUI-6: Terrain component -> heightfield -> mesh. Shared by the runtime viewport (renderer registration),
// the Terrain panel (sculpting, ray picks) and tests.

#include <fuse/ecs/components/terrain.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/terrain/heightfield.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::editor {

/// Triangle list in the terrain's local space (XZ centred on the entity), ready for the meshlet builder.
struct TerrainGeometry {
    std::vector<f32> positions; ///< xyz
    std::vector<f32> normals;   ///< xyz
    std::vector<f32> uvs;       ///< uv (0..1 over the terrain)
    std::vector<u32> indices;
    f32 lo[3] = {0.f, 0.f, 0.f};
    f32 hi[3] = {0.f, 0.f, 0.f};
};

/// Engine mesh ids reserved for terrains (below MeshRegistry::kFirstAssetEngineId): base + entity index.
inline constexpr u32 kTerrainEngineMeshBase = 0xC000u;
inline constexpr u32 kTerrainEngineMeshCount = 0x3000u;
[[nodiscard]] inline u32 terrainEngineMeshId(ecs::EntityID e) { return kTerrainEngineMeshBase + e.index % kTerrainEngineMeshCount; }

/// Clamp `t` to the supported ranges (cells 2..256, size > 0, octaves 1..6, stamp_count <= kMaxStamps).
[[nodiscard]] ecs::Terrain sanitizedTerrain(const ecs::Terrain& t);
/// Heights (noise + stamps) on a (cells + 1)^2 heightfield whose world origin is the terrain's -X/-Z corner.
void buildTerrainHeightfield(const ecs::Terrain& t, terrain::Heightfield& out);
/// Mesh of the terrain via fuse::terrain::build_chunk_mesh, recentred so the entity sits in the middle.
bool buildTerrainGeometry(const ecs::Terrain& t, TerrainGeometry& out);
/// Local height at local (x, z) (bilinear; clamped at the border).
[[nodiscard]] f32 terrainHeightAt(const terrain::Heightfield& field, const ecs::Terrain& t, f32 x, f32 z);
/// Append a sculpt stamp; false when the stamp budget (kMaxStamps) is full.
bool addTerrainStamp(ecs::Terrain& t, const ecs::TerrainStamp& stamp);
/// Ray (local space) vs heightfield: march + bisection. Returns true with the local hit point.
bool raycastTerrain(const terrain::Heightfield& field, const ecs::Terrain& t, const f32 origin[3], const f32 dir[3],
                    f32 hit[3]);
/// FNV-1a of the component bytes (change detection for mesh rebuilds).
[[nodiscard]] u64 terrainHash(const ecs::Terrain& t);

} // namespace fuse::editor
