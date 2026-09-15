# Track B — Terrain System (B7.5 deepen)

**Status:** B7.5 deepen — clipmap LOD ring transitions and vertex morph stub landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.5  
**Source narrative:** [P7.md](../sources/P7.md) §7.5

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `TerrainDesc` / `TerrainChunk` | `Source/FUSE/Terrain/include/fuse/terrain/terrain_desc.hpp` | Resolution, world size, LOD count; `morph_factor` per chunk |
| `Heightfield` | `Source/FUSE/Terrain/include/fuse/terrain/heightfield.hpp` | CPU heightmap, bilinear `sample_height` / `sample_normal` |
| `LodLevel` / `LodTransition` | `Source/FUSE/Terrain/include/fuse/terrain/lod.hpp` | Distance-based LOD rings + morph band |
| `morph_vertex_position` | `Source/FUSE/Terrain/src/lod.cpp` | CPU stub — snap XZ toward coarser grid |
| `ChunkGrid` | `Source/FUSE/Terrain/include/fuse/terrain/chunk_grid.hpp` | Clipmap ring LOD update with morph-factor tracking |
| `sample_height` / `raycast_heightfield` | `Source/FUSE/Terrain/include/fuse/terrain/queries.hpp` | Query APIs for gameplay and physics |
| `Terrain` | `Source/FUSE/Terrain/include/fuse/terrain/terrain.hpp` | Facade: generate, deform, LOD, visible chunks |

**Not in scope:** GPU heightmap textures, vertex-shader displacement, `fuse_scene::SVO` cave integration, async chunk IO, runtime mesh rebuild, legacy Torque terrain cook.

---

## Design

### Heightfield surface

The surface is a regular grid of `f32` heights in metres. `Heightfield::sample_height` bilinearly interpolates world XZ coordinates. `Terrain::generate` fills the grid with deterministic hash noise for smoke tests.

### Clipmap LOD ring transitions

`compute_lod_transition` maps camera-to-chunk distance to a discrete LOD ring and a morph factor:

| Field | Meaning |
|-------|---------|
| `lod` | Discrete ring index (0 = finest) |
| `morph_factor` | [0, 1] blend toward coarser vertex grid in the outer 25% of each ring |

Ring width is 8 m (stub constant). `select_lod_level` delegates to `compute_lod_transition` for backward compatibility.

### Vertex morph stub

`morph_vertex_position` snaps vertex XZ toward the coarser LOD grid:

```
grid = base_stride * 2^lod
morphed = lerp(position, round(position / grid) * grid, morph_factor)
```

Y (height) is preserved — GPU heightmap displacement handles vertical detail later. `ChunkGrid::update_lod` writes `morph_factor` onto each `TerrainChunk` and marks chunks `dirty` when LOD changes or morph is active.

### Queries

`raycast_heightfield` marches along a ray in world-space steps derived from texel size and reports the first crossing below the heightfield surface.

### Future unification

| Legacy | FUSE target |
|--------|-------------|
| `Engine/source/terrain/*` heightfield blocks | `fuse::terrain::Heightfield` + GPU displacement via `fuse_rhi` |
| Underground / cave volumes | `fuse::scene::SVO` carved below the heightfield (B3.5) |
| Editor terrain tools | U6 editor commands over `Terrain::deform` / layer paint |

---

## Build

`fuse_terrain` builds with the FUSE umbrella (always on). Tests register as `fuse_terrain_b75`.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_terrain
```

---

## Tests

`Source/FUSE/Terrain/tests/test_terrain.cpp` (`fuse_terrain_b75`):

| Test | Gate |
|------|------|
| `testHeightfieldSampling` | Bilinear height and flat normal |
| `testLodSelection` | Distance-based LOD and stride growth |
| `testLodTransitionMorphBand` | Morph factor active in ring outer band |
| `testVertexMorphSnapsToGrid` | Full morph snaps XZ to coarser grid |
| `testChunkGridLodTransitions` | Chunks carry morph factors after `update_lod` |
| `testHeightfieldRaycast` | Ray hits flat heightfield |
| `testTerrainFacade` | Generate, deform, raycast, visible chunks with morph |

---

## Gates (B7.5 deepen)

- [x] `LodTransition` struct with LOD ring + morph factor
- [x] `compute_lod_transition` clipmap ring heuristic
- [x] `morph_vertex_position` CPU stub (XZ snap toward coarser grid)
- [x] `ChunkGrid` tracks per-chunk `morph_factor`
- [x] `fuse_terrain_b75` CTest target green
- [x] No owning raw pointers in public FUSE APIs

---

## References

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.5
- [P7.md](../sources/P7.md) §7.5
- Torque clipmap terrain in `Engine/source/terrain/`
