#pragma once

#include <fuse/types.hpp>

namespace fuse::ecs {

/// One sculpt edit on a Terrain, in the terrain's local XZ (metres, centred on the entity).
struct TerrainStamp {
    f32 x = 0.f;
    f32 z = 0.f;
    f32 radius = 4.f;
    f32 amount = 1.f; ///< metres raised / lowered at the centre; Flatten: target local height
    u32 mode = 0;     ///< TerrainStamp::Raise / Lower / Flatten
    enum : u32 { Raise = 0, Lower = 1, Flatten = 2 };
};

/// Heightfield terrain (GUI-6). The surface is regenerated deterministically from these fields: fractal
/// value noise (fuse::terrain::generate_terrain_heights) over a `cells` x `cells` grid `size` metres wide,
/// centred on the entity, then the sculpt stamps in order. Plain data, so scenes save it like any component;
/// the editor builds the mesh (fuse::terrain::build_chunk_mesh) and registers it with the renderer.
struct Terrain {
    static constexpr const char* component_name = "Terrain";
    static constexpr u32 kMaxStamps = 256;

    u32 cells = 64;      ///< quads per edge (2..256)
    f32 size = 64.f;     ///< world metres per edge
    f32 height = 6.f;    ///< noise amplitude (metres); 0 = flat
    u32 seed = 1;
    u32 octaves = 4;
    u32 material_id = 0;
    u32 stamp_count = 0;
    TerrainStamp stamps[kMaxStamps]{};
};

} // namespace fuse::ecs
