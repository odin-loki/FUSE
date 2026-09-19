# FUSE U0 — Work Plan (Unification + Parallel Foundations)

**Phase:** U0 planning deliverable  
**Date:** 2026-09-15  
**Horizon:** Now → U8 exit + Track A P3 job spine in parallel with U1–U4  
**Architecture:** [architecture-parallel.md](./architecture-parallel.md)  
**Evidence:** [concurrency-inventory.md](./concurrency-inventory.md)  
**Platform policy (locked):** Desktop **and** mobile (iOS/Android) from start — adaptive workers, lifecycle-aware pools, portable GFX threading rules.

**Effort bands:** **S** (days, 1 agent) · **M** (1–2 weeks) · **L** (multi-week, multi-stream) · **XL** (phase gate)

---

## 1. Workstream map (honest parallelism)

```
Stream A — Build / link spine     U1 ──► U2 ──► umbrella CI
Stream B — FUSE core + jobs       U1 stub ──► U3 P1-P3 jobs (parallel to U2)
Stream C — Dimensions + hybrid    U4 (after U2 smoke + job API sketch)
Stream D — Feature modules        U5 (after U4 APIs; modules parallelizable)
Stream E — Editor Qt              U6 (overlaps U5; needs game thread model)
Stream F — Content / converters   U7
Stream G — Parity demos           U8
Stream H — Docs / gates           U0 ✓ ──► ongoing
```

| Streams that can run **in parallel** (after deps met) | Prerequisite |
|-----------------------------------------------------|--------------|
| B + A after U1 | Umbrella CMake exists |
| D (per module) after U4 | `IDimension` + handles stable |
| E + D after U4 | Game thread / Editor API boundary defined |
| H anytime | — |

---

## 2. Ordered work packages

### WP-00 — U0 inventory & architecture (this PR)

| Field | Value |
|-------|-------|
| **Effort** | M (complete) |
| **Deliverables** | Collision report, subsystem matrix, addon catalog, demos, risks, layout, merge strategy, **concurrency inventory**, **architecture-parallel**, **work-plan** |
| **Exit** | All docs in `docs/unification/`; stakeholder defaults documented |
| **Deps** | Submodules init |

---

### WP-01 — U1 Umbrella build

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | Root CMake FUSE umbrella; `FUSE_BUILD_T3D`, `FUSE_BUILD_T2D`; `docs/unification/BUILD.md`; CI configure both targets |
| **Parallel** | Stream A; can start **B stub** (`Source/FUSE/CMakeLists.txt` empty options) |
| **Exit** | One `cmake` configure builds T3D app + T2D engine target; zero secret scripts |
| **Deps** | WP-00 |
| **Status** | ✅ Done (U1 PR) — umbrella CMake, `fuse_core` target, BUILD.md, Linux + Android CI |

---

### WP-02 — U2 One-process smoke + legacy quarantine

| Field | Value |
|-------|-------|
| **Effort** | L |
| **Scope** | `fuse_t3d_legacy`, `fuse_t2d_legacy` static libs with symbol prefix; `fuse_runtime_smoke`; ASan clean init |
| **MT note** | Single-threaded smoke OK; **no** parallel tick yet |
| **Exit** | Core stub + both legacy inits in one process; documented remaining symbol conflicts trending down |
| **Deps** | WP-01 |
| **Status** | 🚧 Incremental — Con:: **33/33** shimmed; Engine probe + SimObject bridge slice + owned StringInternTable; full Engine init blocked (see [U2-SMOKE.md](./U2-SMOKE.md)) |

---

### WP-03 — Job scheduler spine (Track A P3 / B1.5)

| Field | Value |
|-------|-------|
| **Effort** | L |
| **Scope** | `Source/FUSE/Core/jobs/` + `Core/platform/` — fiber pool, `JobCounter`, `parallel_for`, **`computeWorkerCount()`** (desktop + mobile profiles), single-thread fallback; unit tests |
| **Parallel** | **Runs parallel to WP-02** once `fuse_core` stub lands (U1) |
| **Mobile** | iOS + Android CI compile of job stubs; adaptive `N` max 4; 32 KiB fiber stacks on mobile |
| **Emscripten** | Stub profile only (`FUSE_JOBS_SINGLE_THREAD`) — **does not gate** WP-03 exit |
| **Exit** | Job tests pass on Linux + **one mobile target** (iOS sim or Android NDK); work-stealing on 4+ core desktop; `FUSE_JOBS_SINGLE_THREAD` works; background reduces `N` |
| **Deps** | WP-01 (cmake target); architecture-parallel §3.1–3.6 |
| **Status** | ✅ Cooperative fiber wait on POSIX (Linux CI); Win/Emscripten backends deferred — see [wp03-fiber-remaining.md](./wp03-fiber-remaining.md) |

---

### WP-04 — U3 Shared services + MT rules

| Field | Value |
|-------|-------|
| **Effort** | L |
| **Scope** | Logger, VFS, handles (`fuse::Handle`), allocators (frame linear), string intern (game thread); I/O lane |
| **MT note** | Publish asset handles from I/O jobs; game thread commit; TSan nightly begins |
| **Exit** | Both dims log via FUSE logger; one asset loaded via VFS from job; handle rules documented |
| **Deps** | WP-02, WP-03 (partial — I/O can use thread pool before fibers complete) |
| **Status** | ✅ Done — logger (both dims via `fuse::log`), HandleTable publish/commit, VFS async I/O lane, U3 gate tests; see [handle-rules.md](./handle-rules.md), [vfs-mount-plan.md](./vfs-mount-plan.md) |

---

### WP-05 — Scene hierarchy greenfield (2D→3D merge)

| Field | Value |
|-------|-------|
| **Effort** | L |
| **Scope** | `fuse::Object`, `SceneObject2D`, `SceneObject3D`, adapters to legacy; snapshot SOA |
| **Parallel** | Overlaps late WP-04 |
| **Exit** | Unit tests for hierarchy; adapter round-trip one legacy object; **no** cross-thread raw pointers |
| **Deps** | WP-04 handles; [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md) |
| **Status** | ✅ Done — hierarchy + snapshot SoA tests; T2D/T3D legacy stub round-trip; handle-only worker reads — see [WP-05-SCENE-HIERARCHY.md](./WP-05-SCENE-HIERARCHY.md) |

---

### WP-06 — U4 Dimension APIs + hybrid frame

| Field | Value |
|-------|-------|
| **Effort** | XL |
| **Scope** | `World2D`, `World3D`, `IDimension`, `HybridComposer`; parallel cull `parallel_for`; game-thread physics |
| **MT note** | Frame barrier; **portable** render record on `fuse::platform::renderThread()` (game thread v1); GLES + desktop GL |
| **Mobile** | Hybrid demo runs on iOS **or** Android device/sim; respect surface loss / background |
| **Exit** | Demo: 3D clear + spinning 2D sprite one window (desktop + one mobile); TSan clean on cull path |
| **Deps** | WP-05, WP-03 |
| **Status** | 🚧 Core frame green — `fillSnapshotSoA` in worlds, SoA cull, barrier, software `demo_hybrid_hud`; Track B bindless composite GPU blit ✅ (WP-06f) — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) |

---

### WP-06b — Track B Vulkan bootstrap (B2.1–B2.2)

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `fuse_rhi` instance/device/swapchain (headless + External surface path), `RenderCommandList`, `PresentPath`, `gl_context.hpp`, Hybrid dual-path wiring, `FUSE_BUILD_VULKAN` |
| **MT note** | Submit on `renderThread()` only; workers stay snapshot-only |
| **Exit** | Headless bootstrap tests green; CI honest stub/Lavapipe story documented (umbrella CI not re-enabled); U4 placeholder renderer unchanged |
| **Deps** | WP-06 scaffolding |
| **Status** | ✅ B2.1 instance/device + B2.2 swapchain/frame ring + present-path stubs landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06b |

---

### WP-06c — Track B queue submit + honest present (B2.2 follow-up)

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `submitGraphicsQueue`, `RhiContext` frame-slot submit, `HybridRendererBootstrap` acquire→render→present ordering, headless Lavapipe path, `fuse_rhi_queue_submit` |
| **MT note** | `vkQueueSubmit` on `renderThread()` only; headless CI skips `vkQueuePresentKHR` with explicit diagnostics |
| **Exit** | Real `vkQueueSubmit` under Lavapipe; hybrid bootstrap + phase2 integration assert submit count; desktop WSI/Qt deferred |
| **Deps** | WP-06b |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06c |

---

### WP-06d — Track B render encode + bindless pool + null WSI (B2.4–B2.5 follow-up)

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | Real `vkCmdBeginRenderPass` in render-graph execute; bindless `VkDescriptorPool` + in-memory `PipelineCache`; null/GLFW WSI scaffold (`FUSE_PLATFORM_WINDOW_GLFW`); keep `HybridRendererBootstrap` + `demo_hybrid_hud` green |
| **MT note** | GPU encode + submit on `renderThread()` only |
| **Exit** | `fuse_vulkan_phase2_integration` asserts `vulkanRenderPassBeginCount`; bindless pool test; hybrid presentable WSI scaffold test; Lavapipe headless unchanged |
| **Deps** | WP-06c |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06d |

---

### WP-06e — Track B graph barriers + bindless updates + cache I/O + swapchain present FB

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | Graph-planned `vkCmdPipelineBarrier`; bindless `vkUpdateDescriptorSets` on register/unregister; pipeline cache disk serialize/restore; swapchain per-image FB + present-pass encode when WSI acquire valid; keep null WSI + Lavapipe headless green |
| **MT note** | GPU encode + descriptor updates on render thread / resource init path only |
| **Exit** | `fuse_vulkan_phase2_integration` asserts barrier encode; `fuse_bindless_descriptors` descriptor update/clear; `fuse_pipeline_cache` disk round-trip; headless swapchain has no present FB |
| **Deps** | WP-06d |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06e |

---

### WP-06f — Track B bindless composite GPU blit + CUDA interop stubs

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `CompositeGpuPath` bindless composite shader blit into swapchain backbuffer (headless-honest offscreen when no WSI); bindless descriptor set bound in composite pipeline; B2.6 `InteropUnavailableReason` + timeline/import honest stubs + tests that skip cleanly; keep Lavapipe + `demo_hybrid_hud` green |
| **MT note** | Composite encode + bindless registration on render thread only |
| **Exit** | `fuse_vulkan_phase2_integration` asserts `vulkanCompositeDrawCount`; `fuse_cuda_interop` reason strings + timeline stubs; `demo_hybrid_hud` PASS headless |
| **Deps** | WP-06e |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06f |

---

### WP-07 — U5 Feature modules (parallel per module)

| Field | Value |
|-------|-------|
| **Effort** | XL (5 sub-packages) |
| **Sub-packages** | `fuse_ai`, `fuse_cinematics`, `fuse_fx`, `fuse_mechanics`, `fuse_adventure` — **each parallelizable** after WP-06 |
| **MT note** | AI/FX jobify per architecture §7 |
| **Exit** | Per-module U5 gates in prestarter §10 |
| **Deps** | WP-06 |
| **Status** | ✅ Prestarter §10 gates closed; post-gate ore: per-agent tree profiles, UAISK script import, `outpost_intro_30s.seq` loader, ShapeBase VActor attach, GPU particle pool backend, AFX mission hooks, `PhysicsTriggerBridge`, Delay/Rotate GMK leaves, `outpost_loader` + hybrid NPC converse — [U5-MODULES.md](./U5-MODULES.md) |

---

### WP-08 — U6 Qt editor vertical slice

| Field | Value |
|-------|-------|
| **Effort** | L |
| **Scope** | In-process PIE; UI thread vs game thread command queue; one feature pane |
| **Deps** | WP-06, WP-05 |
| **Status** | 🚧 Advancing — inspector props + undo/redo through queue; `RuntimeViewportHook` embed stub; Qt property pane when Qt6 present — [U6-EDITOR.md](./U6-EDITOR.md) |

---

### WP-09 — U7 Project format + converters

| Field | Value |
|-------|-------|
| **Effort** | L |
| **Scope** | `project.json`, importers, cookers under `Tools/FUSE/` |
| **Deps** | WP-06 |
| **Status** | 🚧 Advancing — loader + importers + `.fuselevel` v2 hierarchy + `fuse_convert` / `fuse_cook --fuselevel` stubs — [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md) |

---

### WP-10 — U8 Parity demos

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `Samples/unification/demo_*` per [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) |
| **Deps** | WP-07 (subset), WP-09 |
| **Status** | ✅ Minimum set — seven `project.json` stubs + headless demo binaries wired in CI |

---

### WP-11 — Legacy parallel audit (continuous)

| Field | Value |
|-------|-------|
| **Effort** | S per sprint |
| **Scope** | Route safe T3D/T2D loops to jobs ([FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) B1.5 table); shrink `_forceAllMainThread` reliance |
| **Deps** | WP-03 |
| **Status** | 🚧 P1 landed — quarantine `compressMipsParallel` routes Tier A mip loop via `parallel_for_indices`; `imageUtils.cpp` swap deferred (`FUSE_T3D_LEGACY_ENGINE_PROBE` bitmapUtils only; SimObject ODR); Con:: shims **33/33**; P2 terrain cell VB next |

---

## 3. Dependency graph (critical path)

```
WP-00 → WP-01 → WP-02 ──────────────────────────────┐
              └→ WP-03 (jobs) ──┐                    │
                                ▼                    │
                         WP-04 (services)            │
                                ▼                    │
                         WP-05 (scene 2D→3D)       │
                                ▼                    │
                         WP-06 (U4 hybrid) ◄─────────┘
                                ├→ WP-07 (modules, parallel)
                                ├→ WP-08 (editor)
                                └→ WP-09 → WP-10
```

**Critical path:** WP-00 → 01 → 02 → 04 → 05 → 06 → 10

**Parallel win:** WP-03 alongside WP-02; WP-07 modules in parallel after WP-06.

---

## 4. Phase gates × MT criteria

| Gate | Unification | MT add-on criterion |
|------|-------------|---------------------|
| **U1** | Umbrella build | `fuse_core` cmake target exists |
| **U2** | One-process smoke | ASan init; jobs optional — ✅ smoke + quarantine libs |
| **U3** | Shared services | I/O job publishes handle; TSan plan live |
| **U4** | Hybrid demo | `parallel_for` cull + SoA; frame barrier; software demo ✅ — real GLES/Vulkan present ❌ Track B |
| **U5** | Feature modules | Five `fuse_*` targets; `fuse_ai` BT slice + tests — 🚧 scaffolds + ore backlog ([U5-MODULES.md](./U5-MODULES.md)) |
| **U6** | Editor PIE | UI/game thread queue + inspector/undo through queue + runtime viewport embed stub — 🚧 GPU viewport remains |
| **U7** | Project format | `fuse_project` + `.fuselevel` v2 hierarchy + converters/cook stubs — 🚧 full asset pipeline deferred |
| **U8** | Parity demos | Seven demo binaries + `Samples/unification/` stubs — ✅ minimum set |
| **P3 (Track A)** | Job system tests | Fiber scheduler + single-thread fallback |

---

## 5. Team / agent parallelization guide

| Agent / team | After gate | Focus |
|--------------|------------|-------|
| **Agent A** | U1 | CMake, CI, legacy wrap — **include iOS/Android toolchain matrix** |
| **Agent B** | U1 | `fuse_core` + jobs + **`fuse/platform`** (WP-03) |
| **Agent C** | U3 | Scene types + adapters (WP-05) |
| **Agent D** | U4 | World2D OR World3D (split) |
| **Agent E** | U5 | One addon module each |
| **Agent F** | U6 | Qt editor shell (desktop) |
| **Agent G** | U3 | Mobile lifecycle hooks (background worker drain) |
| **Docs** | U0+ | Keep inventory current when code lands |

**Merge rule:** All product code obeys [architecture-parallel.md](./architecture-parallel.md) §5–6 before parallel scene tick merges.

---

## 6. Immediate next 5 actions (after this PR merges)

1. ✅ **Locked:** Platform scope = desktop + mobile — worker adaptive formula implemented in `fuse::jobs::computeWorkerCount()`.

2. ✅ **WP-01 (U1):** Umbrella CMake + [BUILD.md](./BUILD.md) — Win/Linux/macOS + iOS + Android paths; `FUSE_PLATFORM_*` options.

3. ✅ **WP-03 stub:** `Source/FUSE/Core/include/fuse/jobs/` + `fuse/platform/` — stubs + worker-count tests (`fuse_core_tests`).

4. ✅ **WP-02 prep:** [Tools/FUSE/prefix_legacy_symbols.py](../../Tools/FUSE/prefix_legacy_symbols.py) — dry-run prefix planner.

5. ✅ **CI:** `.github/workflows/fuse-umbrella-linux.yml` + `fuse-core-android.yml`; iOS stub in `fuse-core-ios.yml` (macOS manual/dispatch).

**Next:** U2 incremental — expand Engine probe (gfx/platform stubs), SimObject adapter → first curated `.cpp` batch, StringTable route to FUSE core; U6 GPU viewport embed; U7 full T3D field extraction + production asset cooks; Track B post–WP-06f (`vkQueuePresentKHR` desktop GLFW, Editor Qt surface, `cudaImportExternalMemory`).

---

## 7. Risk cross-reference

| Risk | Work package mitigation |
|------|-------------------------|
| R14 Static init | WP-02 explicit init order in `fuse_core` |
| R16 Inheritance misuse | WP-05, WP-06 code review checklist |
| R17 Data races on scene graph | WP-04 handles, WP-05 snapshots, WP-06 barrier |
| R01 Symbol collision | WP-02 prefix libs |
| R02 Dual script VMs | WP-04 script host decision |

---

## 8. Document maintenance

Update this plan when:
- U gates pass (check off WPs)
- Stakeholder overrides defaults in architecture §12
- New evidence from legacy datamine changes assumptions
