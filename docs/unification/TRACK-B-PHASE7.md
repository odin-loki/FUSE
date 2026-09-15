# Track B — Phase 7 Deliverables & Integration (B7.10)

**Status:** B7.10 checklist registry + cross-module integration smoke landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.10  
**Depends on:** B7.1–B7.9 subsystem scaffolds (Track A P7 narrative)

---

## B7.1–B7.9 status summary

| ID | System | Library / location | Stub landed | Narrative tests | Notes |
|----|--------|-------------------|-------------|-----------------|-------|
| **B7.1** | Animation | `fuse_animation` | ✅ | `fuse_animation_runtime` | CPU skeleton, blend tree, IK, skinning, `Animator` facade |
| **B7.2** | Spatial audio | `fuse_audio` | ✅ | `fuse_audio_b72` | OpenAL when available; null backend fallback; CPU spatial mix + reverb facade |
| **B7.3** | Scripting | `fuse_script` | ✅ | `fuse_script_b73` | Lua-ready `ScriptHost` / null `ScriptVM`; ECS bind helpers |
| **B7.4** | Networking | `fuse_net` | ✅ | `fuse_net_b74` | Loopback transport, serializer, rollback + state-sync stubs |
| **B7.5** | Terrain | `fuse_terrain` | ✅ | `fuse_terrain_b75` | Heightfield, chunk LOD grid, `Terrain` facade |
| **B7.6** | World partition | `fuse_world_partition` | ✅ | `fuse_world_partition_b76` | Grid cells, residency eviction/budget counters, unload priority stubs, JobScheduler async queue |
| **B7.7** | VFX | `fuse_vfx` | ✅ | `fuse_vfx_runtime` | CPU `ParticleSystem`, emitters, effect instances |
| **B7.8** | Platform hardening | `fuse_core` (`fuse::platform`) | ✅ | `fuse_core_platform_hardening` | Lifecycle, surface-loss, crash-report, desktop/mobile profile hooks |
| **B7.9** | Asset pipeline | `fuse_project` + `Tools/FUSE` | ✅ | `fuse_assets_b79` | Cook manifest, `AssetGraph`, content-hash cache (path+mtime+bytes keys), `ImportPipeline`, `fuse_cook` / `fuse_import` CLI |

Submodule detail:

| Topic | Doc / README |
|-------|----------------|
| B7.1 Animation | [TRACK-B-ANIMATION.md](./TRACK-B-ANIMATION.md), [Source/FUSE/Animation/README.md](../../Source/FUSE/Animation/README.md) |
| B7.2 Audio | `Source/FUSE/Audio/` (see `fuse_audio_b72` tests) |
| B7.3 Script | [TRACK-B-SCRIPT.md](./TRACK-B-SCRIPT.md) |
| B7.4 Net | [Source/FUSE/Net/README.md](../../Source/FUSE/Net/README.md), [TRACK-B-NET.md](./TRACK-B-NET.md) |
| B7.5 Terrain | [TRACK-B-TERRAIN.md](./TRACK-B-TERRAIN.md) |
| B7.6 World partition | [Source/FUSE/WorldPartition/README.md](../../Source/FUSE/WorldPartition/README.md), [TRACK-B-WORLD-PARTITION.md](./TRACK-B-WORLD-PARTITION.md) |
| B7.7 VFX | [TRACK-B-VFX.md](./TRACK-B-VFX.md), [Source/FUSE/VFX/README.md](../../Source/FUSE/VFX/README.md) |
| B7.8 Platform | [TRACK-B-PLATFORM.md](./TRACK-B-PLATFORM.md) |
| B7.9 Assets | [TRACK-B-ASSETS.md](./TRACK-B-ASSETS.md) |

---

## B7.10 — Deliverable checklist registry

`Phase7TestRegistry` (`Source/FUSE/Phase7/`) catalogs master-plan acceptance items per module. Entries record whether the stub scaffold landed and whether an automated probe exists today. Full GPU, ENet, Lua, and shipping-build gates from §B7.10 remain `automated=false` until upstream implementations land.

```cpp
const auto& checklist = fuse::phase7::Phase7TestRegistry::checklist();
fuse::phase7::Phase7TestRegistry::runIntegrationSmoke(); // constructs linked facades
```

---

## B7.10 — Cross-module integration smoke

`fuse_phase7_integration` links optional modules when CMake options are enabled and constructs their public facades in one process:

| Module | Facade exercised | CMake gate |
|--------|------------------|------------|
| B7.1 Animation | `Animator::tick` after state-machine wiring | always |
| B7.2 Audio | `AudioEngine::init` / `destroy` | `FUSE_BUILD_AUDIO` |
| B7.3 Script | `ScriptHost::init` / `shutdown` | `FUSE_BUILD_SCRIPT` |
| B7.4 Net | `LoopbackTransport` init + linked peer send/receive | always |
| B7.5 Terrain | `Terrain::init` / `generate` / `get_height` | always |
| B7.6 World partition | `WorldPartition::init` / `update` | always |
| B7.7 VFX | `ParticleSystem::init` / `spawn_effect` / `update` | always |
| B7.8 Platform | `currentJobProfileLimits()` after `core::initialize` | always (`fuse_core`) |
| B7.9 Assets | `ImportPipeline::plan_from_manifest` dry-run | `FUSE_BUILD_PROJECT` |

No Torque legacy, GPU device, or OpenAL output is required — the smoke validates facade lifecycles and stub data paths only.

---

## Build

`fuse_phase7` builds whenever `FUSE_BUILD_CORE=ON` (default). Tests register when `FUSE_BUILD_CORE_TESTS=ON`.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_AUDIO=ON \
  -DFUSE_BUILD_SCRIPT=ON \
  -DFUSE_BUILD_PROJECT=ON

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_phase7
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_AUDIO=OFF` | Audio facade skipped; checklist still lists B7.2 |
| `FUSE_BUILD_SCRIPT=OFF` | Script facade skipped |
| `FUSE_BUILD_PROJECT=OFF` | Asset pipeline facade skipped |
| `FUSE_BUILD_CORE_TESTS=OFF` | No `fuse_phase7_integration` CTest target |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_phase7_integration` | Checklist non-empty, per-module counts, `runIntegrationSmoke()` constructs linked facades |

Run:

```bash
ctest --test-dir build --output-on-failure -R fuse_phase7
```

---

## Gates (B7.10 scaffold)

- [x] B7.1–B7.9 status documented with stub/test pointers
- [x] `Phase7TestRegistry` checklist on FUSE APIs
- [x] Cross-module integration smoke constructs linked facades
- [x] CTest target green in Linux umbrella CI
- [ ] Master-plan §B7.10 full acceptance matrix (GPU skinning, ENet processes, Lua hot-reload, shipping strip, etc.) — deferred per-module

---

## CI story (honest)

1. **Linux umbrella** — all B7 module tests plus `fuse_phase7_integration` run in the Release CTest job.
2. **ASan smoke** — `fuse_runtime_smoke` remains separate; phase-7 integration runs in the umbrella job (same pattern as B4.11).
3. **Optional modules** — umbrella CI enables `FUSE_BUILD_AUDIO`, `FUSE_BUILD_SCRIPT`, and `FUSE_BUILD_PROJECT` so all nine facades are exercised.

---

## Next

- [ ] Wire phase-7 acceptance items to real backends as B7.1–B7.9 implementations mature
- [ ] Extend integration smoke with ECS registry + `PhysicsManager` tick (cross-phase with B4.11)
- [ ] Add process-pair ENet smoke when transport backend leaves stub mode
- [ ] Shipping-build gate (`FUSE_SHIPPING`) when platform hardening lands OS handlers

---

## Related docs

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.1–B7.10
- [TRACK-B-PHYSICS.md](./TRACK-B-PHYSICS.md) — B4.11 phase-4 deliverable pattern
- [BUILD.md](./BUILD.md) — umbrella CMake options
