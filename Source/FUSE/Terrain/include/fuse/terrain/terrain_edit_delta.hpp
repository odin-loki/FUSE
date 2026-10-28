#pragma once

#include <fuse/terrain/math.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::terrain {

/// Metres added to one generated height texel. Absent texels stay at the base height.
struct TerrainHeightEdit {
    u32 x = 0;
    u32 z = 0;
    f32 delta = 0.f;
};

/// One spherical cave carve. Replayed in order onto a fresh underground volume.
struct TerrainCaveEdit {
    vec3 center{};
    f32 radius = 0.f;
};

/// Sparse edits on top of a procedural heightfield. The full heightmap is not stored.
///
/// `flags` bit `kTerrainEditGeneratedFlag` means `seed` was passed to `Terrain::generate`
/// and height deltas are relative to that surface. With the flag clear, deltas are relative
/// to a flat zero heightfield (init, no generate).
struct TerrainEditDelta {
    u64 seed = 0;
    u32 flags = 0;
    u32 resolution = 0;
    f32 world_size = 0.f;
    f32 max_height = 0.f;
    u32 svo_depth = 0;
    std::vector<TerrainHeightEdit> heights;
    std::vector<TerrainCaveEdit> caves;
};

inline constexpr u32 kTerrainEditDeltaMagic = 0x44455446u; // file bytes 'FTED'
inline constexpr u32 kTerrainEditDeltaVersion = 1u;
inline constexpr u32 kTerrainEditGeneratedFlag = 1u;
/// Height deltas at or below this (metres) are dropped so cancelling an edit stays sparse.
inline constexpr f32 kTerrainHeightEditEpsilon = 1e-6f;

/// Canonical little-endian blob. Height rows are ordered by (z, x) and omit unmodified texels.
[[nodiscard]] std::vector<u8> serialize_terrain_edit_delta(const TerrainEditDelta& delta);

/// False on a truncated, trailing, unordered, or out-of-range blob. `out` is unchanged then.
[[nodiscard]] bool deserialize_terrain_edit_delta(const u8* data, usize size, TerrainEditDelta& out);

} // namespace fuse::terrain
