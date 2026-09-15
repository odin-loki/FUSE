# Track B — Terrain System (B7.5)

**Status:** B7.5 heightfield + chunk LOD grid stub landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.5  
**Source narrative:** [P7.md](../sources/P7.md) §7.5

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `TerrainDesc` / `TerrainChunk` | `Source/FUSE/Terrain/include/fuse/terrain/terrain_desc.hpp` | Resolution, world size, LOD count; GPU handles stubbed as `u64` |
| `Heightfield` | `Source/FUSE/Terrain/include/fuse/terrain/heightfield.hpp` | CPU heightmap, bilinear `sample_height` / `sample_normal` |
| `LodLevel` | `Source/FUSE/Terrain/include/fuse/terrain/lod.hpp` | Distance-based LOD selection stub |
| `ChunkGrid` | `Source/FUSE/Terrain/include/fuse/terrain/chunk_grid.hpp` | Simple chunk grid with clipmap-style LOD update |
| `sample_height` / `raycast_heightfield` | `Source/FUSE/Terrain/include/fuse/terrain/queries.hpp` | Query APIs for gameplay and physics |
| `Terrain` | `Source/FUSE/Terrain/include/fuse/terrain/terrain.hpp` | Facade: generate, deform, LOD, visible chunks |

**Not in scope:** GPU heightmap textures, vertex-shader displacement, `fuse_scene::SVO` cave integration, async chunk IO, runtime mesh rebuild, legacy Torque terrain cook.

---

## Design (B7.5 stub)

### Heightfield surface

The surface is a regular grid of `f32` heights in metres. `Heightfield::sample_height` bilinearly interpolates world XZ coordinates. `Terrain::generate` fills the grid with deterministic hash noise for smoke tests.

### Chunk LOD grid

`ChunkGrid` partitions the world into a 2D grid of `TerrainChunk` records. `update_lod` assigns each chunk an LOD level from camera distance and toggles `loaded` within `world_size` (no disk or GPU streaming yet).

### Queries

`raycast_heightfield` marches along a ray in world-space steps derived from texel size and reports the first crossing below the heightfield surface.

### Future unification

| Legacy | FUSE target |
|--------|-------------|
| `Engine/source/terrain/*` heightfield blocks | `fuse::terrain::Heightfield` + GPU displacement via `fuse_rhi` |
| Underground / cave volumes | `fuse::scene::SVO` carved below the heightfield (B3.5) |
| Editor terrain tools | U6 editor commands over `Terrain::deform` / layer paint |

---

## Tests

`ctest -R fuse_terrain_b75` runs `fuse_terrain_tests`:

- Bilinear height sampling and normals
- LOD level selection and stride growth
- Chunk grid load/unload stub near vs far camera
- Heightfield raycast on flat terrain
- `Terrain` generate / deform / ray_cast / visible chunks
