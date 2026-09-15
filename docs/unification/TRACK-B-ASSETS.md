# Track B — Asset Pipeline (B7.9)

**Status:** B7.9 offline cook/import stubs + job graph + content-hash cache deepen landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.9  
**Source narrative:** [P7.md](../sources/P7.md) §7.9

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| Import descriptors | `Source/FUSE/Project/include/fuse/project/import_desc.hpp` | `MeshImportDesc`, `TextureImportDesc`, `AudioImportDesc` |
| Cook manifest | `Source/FUSE/Project/include/fuse/project/cook_manifest.hpp` | `CookManifest`, `CookManifestEntry`, load/parse helpers |
| `AssetGraph` | `Source/FUSE/Project/include/fuse/project/asset_graph.hpp` | Dependency tracking + dirty scan stub |
| `AssetCooker` | `Source/FUSE/Project/include/fuse/project/asset_cooker.hpp` | Mesh/texture/audio cook stubs |
| `CookJobGraph` | `Source/FUSE/Project/include/fuse/project/cook_job_graph.hpp` | import→process→pack stage graph + dependency edges |
| `CookContentHash` | `Source/FUSE/Project/include/fuse/project/cook_content_hash.hpp` | FNV-1a file+desc hashing for cache keys |
| `CookCache` | `Source/FUSE/Project/include/fuse/project/cook_cache.hpp` | Content-hashed cook output cache + invalidation |
| `ImportPipeline` | `Source/FUSE/Project/include/fuse/project/import_pipeline.hpp` | Project-scoped plan/execute facade |
| `fuse_cook` CLI | `Tools/FUSE/fuse_cook.cpp` | Headless cook dry-run (mirrors `fuse_import` pattern) |

**Not in scope:** Real FBX/GLTF/OBJ mesh cooks, BC texture compression, audio encode, shader SPIR-V batch compile, Qt cook UI, legacy DTS/DIFF converters.

---

## Design (B7.9 stub)

### Cook manifest

`cook_manifest.json` (schema v1) lists planned cooks beside `project.json`. `loadCookManifestFromFile` and `parseCookManifest` accept a minimal JSON reader (no external deps). `makeDefaultCookManifest` seeds mesh/texture entries for smoke tests.

### Asset cooker

`AssetCooker` validates paths and returns `CookRecord` results without writing engine binaries. Existing source files succeed with a stub note; missing sources report `SourceMissing` so CI can distinguish planning from real cooks.

### Cook job graph (B7.9 deepen)

Each manifest asset expands into a three-stage job: **import** (source validation) → **process** (kind-specific stub transform) → **pack** (`cook_entry`). `CookJobGraph::build_from_manifest` records explicit manifest dependencies plus implicit edges when one job's `output_path` feeds another's `source_path`. `execute` topologically orders jobs, runs stages sequentially, and **short-circuits** remaining stages on failure. Dependent jobs are skipped when an upstream job fails. `CookJobGraphExecuteResult` reports `failed_job_id`, `failed_stage`, and `failure_note`; `AssetCooker::cook_manifest` routes through the graph and maps stage summaries into `CookRecord::note`.

### Content-hash cook cache (B7.9 deepen)

`CookContentHash` computes deterministic FNV-1a keys from source file bytes plus import descriptor knobs (`hash_mesh_import`, `hash_texture_import`, `hash_audio_import`, `hash_manifest_entry`). Identical source+desc inputs produce identical hashes; descriptor or file changes alter the key.

`CookCache` stores `CookCacheEntry` records keyed by content hash. `AssetCooker` consults the cache before stub cooks:

- **Miss** — runs the stub cook, stores the entry, sets `CookRecord::cache_hit = false` and appends `(cache miss)` to the note.
- **Hit** — returns the cached output path without re-running stages, sets `CookRecord::cache_hit = true`, increments hit stats.

Invalidation paths:

- `CookCache::invalidate(hash)` — drop one entry by content hash.
- `CookCache::invalidate_source(path)` — drop all entries sourced from a file.
- `CookCache::invalidate_all()` — clear the cache.
- `AssetCooker::cook_dirty` — invalidates cache entries for dirty asset sources before stub reimport.

`CookCacheStats` tracks hits, misses, and invalidations. The cache persists to JSON via `save`/`load` for offline cook follow-up.

### Import pipeline

`ImportPipeline::plan_from_manifest` registers assets in `AssetGraph` and returns a dry-run batch. `execute(dry_run)` either replays the plan or delegates to `AssetCooker::cook_manifest`. `planForProject` ties the default manifest to a loaded `ProjectManifest`.

### Dependency graph

`AssetGraph` tracks output → source mappings, optional dependencies, and last-import timestamps. `scan_for_changes` compares filesystem mtimes; `reimport_dirty` logs stub reimports for incremental cook follow-up.

---

## Build

`fuse_project` builds when `FUSE_BUILD_PROJECT=ON` (default). `fuse_cook` builds when `FUSE_BUILD_TOOLS=ON` and project support is enabled.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_PROJECT=ON \
  -DFUSE_BUILD_TOOLS=ON

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_assets
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_TOOLS=OFF` | `fuse_cook` / `fuse_import` omitted |
| `FUSE_BUILD_PROJECT=OFF` | Asset pipeline library omitted |
| `FUSE_BUILD_CORE_TESTS=ON` | `fuse_assets_b79` CTest target |

---

## CLI

```bash
# Plan default cooks for a U7 project (dry-run)
./build/Tools/FUSE/fuse_cook --project Samples/unification/demo_3d_empty --dry-run

# Load an explicit cook manifest
./build/Tools/FUSE/fuse_cook --manifest path/to/cook_manifest.json --dry-run

# Single-asset stub cooks
./build/Tools/FUSE/fuse_cook --mesh --input art/mesh.obj --output cooked/mesh.fusemesh
```

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_assets_b79` | Cook manifest parse, asset graph save/load, cooker stub, job graph stage ordering + failure short-circuit, content-hash cache hit/miss + invalidation, pipeline dry-run, project plan |

Run:

```bash
ctest --test-dir build --output-on-failure -R fuse_assets_b79
```

---

## Gates (B7.9 stub)

- [x] `AssetCooker` / `ImportPipeline` on FUSE project APIs
- [x] Cook manifest types + minimal JSON loader
- [x] `AssetGraph` dirty scan + persistence stub
- [x] `CookJobGraph` import→process→pack stages + dependency edges + failure reporting
- [x] `CookContentHash` + `CookCache` content-hash keys, hit/miss stats, invalidation hooks
- [x] `fuse_cook` CLI dry-run
- [x] CTest target green in umbrella CI
- [ ] Real mesh/texture/audio encoders (follow-up)
- [ ] Legacy DTS/DIFF compat importers under `Tools/FUSE/Cook/` (follow-up)

---

## Next

- [ ] Wire Assimp/meshoptimizer for mesh cooks
- [ ] BC7/BC5 texture compression path
- [ ] OGG encode + normalise for audio
- [ ] Shader offline SPIR-V batch (`compile_all`)
- [ ] Qt cook UI sharing `ImportPipeline`

---

## Related docs

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.9
- [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md) — `project.json` + `fuse_import`
- [unified-layout.md](./unified-layout.md) — `Tools/FUSE/Cook/` target layout
