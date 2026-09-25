# fuse_terrain — B7.5 Terrain System

CPU heightfield terrain for Track B7.5: seeded fractal noise generation, bilinear height/normal
queries and ray casts, clipmap LOD chunk streaming with crack-free seams, deformation, and SVO caves.
The GPU displacement path and the on-screen cave ray march are not written yet.

`generate → sample height → LOD chunk grid → chunk meshes (+ SVO caves) → raycast`

## Layout

| Header | Role |
|--------|------|
| `terrain_desc.hpp` | `TerrainDesc`, `TerrainChunk` configuration and chunk metadata |
| `heightfield.hpp` | CPU heightmap storage with bilinear sampling |
| `terrain_noise.hpp` | Seeded fractal value noise (quintic fade, C2 across lattice cells) used by `Terrain::generate` |
| `chunk_mesh.hpp` | CPU chunk mesh (reference of the vertex-shader displacement / collision) with crack-free edge stitching against coarser neighbours |
| `terrain_caves.hpp` | Underground caves in the B3.5 SVO (`fuse_scene`, when `FUSE_BUILD_PROJECT`); hybrid heightfield→SVO ray march |
| `lod.hpp` | LOD ring table, transition morph band, vertex morph stub |
| `lod_residency_queue.hpp` | JobScheduler-backed async chunk load/unload queue |
| `chunk_grid.hpp` | Chunk grid with clipmap LOD transitions, morph factors, and residency queue |
| `queries.hpp` | `sample_height` and `raycast_heightfield` query APIs |
| `terrain.hpp` | Game-thread terrain facade (generate, deform, LOD, chunk meshes, caves, queries). `deform` marks touched chunks; the next `update_lod` rebuilds exactly those meshes |

## Tests

`fuse_terrain_tests` (`ctest` name `fuse_terrain_b75`) covers heightfield bilinear sampling, LOD selection, chunk grid streaming stub, heightfield raycast, and the `Terrain` facade without GPU or editor dependencies.

`fuse_b7_terrain_gates_tests` (`ctest` name `fuse_b7_terrain_gates`, labels `perf;gate`, RUN_SERIAL) proves the B7.5 / B7.10 terrain rows: 4096² generation within analytic slope/curvature bounds, `get_height` vs texel reads, normals/ray casts vs analytic surfaces, LOD streaming with no missing geometry (sync + async), crack-free seams across LODs, deform → mesh rebuild within one frame, SVO cave ray-march transition, determinism and the `update_lod` frame budget (enforced under `NDEBUG`).

## Upstream / downstream

- **Depends on:** `fuse_core`; `fuse_scene` (SVO caves) when `FUSE_BUILD_PROJECT` is on
- **Future:** `fuse_rhi` for GPU heightmap displacement and the on-screen SVO cave ray march (B2)
