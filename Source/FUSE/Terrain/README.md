# fuse_terrain — B7.5 Terrain System (stub)

CPU-first heightfield terrain scaffolding for Track B7.5. Implements the P7 pipeline stages as stubs:

`generate → sample height → LOD chunk grid → raycast`

## Layout

| Header | Role |
|--------|------|
| `terrain_desc.hpp` | `TerrainDesc`, `TerrainChunk` configuration and chunk metadata |
| `heightfield.hpp` | CPU heightmap storage with bilinear sampling |
| `lod.hpp` | LOD ring table, transition morph band, vertex morph stub |
| `lod_residency_queue.hpp` | JobScheduler-backed async chunk load/unload queue |
| `chunk_grid.hpp` | Chunk grid with clipmap LOD transitions, morph factors, and residency queue |
| `queries.hpp` | `sample_height` and `raycast_heightfield` query APIs |
| `terrain.hpp` | Game-thread terrain facade (generate, deform, LOD, queries) |

## Tests

`fuse_terrain_tests` (`ctest` name `fuse_terrain_b75`) covers heightfield bilinear sampling, LOD selection, chunk grid streaming stub, heightfield raycast, and the `Terrain` facade without GPU or editor dependencies.

## Upstream / downstream

- **Depends on:** `fuse_core`
- **Future:** `fuse_scene::SVO` for underground caves (B3.5), `fuse_rhi` for GPU heightmap displacement (B2)
