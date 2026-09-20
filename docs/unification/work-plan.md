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
| **Status** | 🚧 Core frame green — `fillSnapshotSoA` in worlds, SoA cull, barrier, software `demo_hybrid_hud`; Track B bindless composite GPU blit ✅ (WP-06f); CUDA interop/GLFW present/U6 surface handoff ✅ (WP-06g); CUDA interop fill + frame-sync progress + Qt surface stub ✅ (WP-06h); Qt `QVulkanInstance` bootstrap + load-stress stubs ✅ (WP-06i); Lavapipe teardown hardening + Qt embed/timeline stress ✅ (WP-06j); Lavapipe tune + composite SPIR-V regen + viewport swapchain recreate stubs ✅ (WP-06k); spirv-val regen gate + consumed swapchain present + PlaceholderRenderer toggle ✅ (WP-06l); Qt `vkQueuePresentKHR` gate + embed PlaceholderRenderer deepen + mobile Vulkan cmake stubs ✅ (WP-06m); Qt present path readiness + combined CUDA/timeline stress ✅ (WP-06n); Qt present path eligible diagnostics + scoped PlaceholderRenderer retirement deepen ✅ (WP-06o); mobile Vulkan stubs deepen + PlaceholderRenderer retirement tests + Lavapipe destroy-order ICD lock ✅ (WP-06p) — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) |

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

### WP-06g — Track B CUDA interop deepen + GLFW present + U6 surface handoff

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | Strengthen `cudaImportExternalMemory` / `SharedTimeline` toward real driver imports when toolkit+extensions present (skip-clean otherwise); CUDA texture bindless path in composite when interop available (shader placeholder honest otherwise); `FUSE_ENABLE_GLFW_PRESENT` gate for `vkQueuePresentKHR` when display+GLFW; `RuntimeViewportHook` → `SwapchainDesc.surface` handoff stubs (U6); keep Lavapipe + `demo_hybrid_hud` green |
| **MT note** | Interop/composite on render thread; editor handoff posted from UI, consumed on game thread |
| **Exit** | `fuse_cuda_interop` FrameSyncPair + import reason tests; `fuse_hybrid_vulkan_presentable` desktop-present gate; `fuse_editor_host` surface handoff; headless CI unchanged |
| **Deps** | WP-06f |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06g |

---

### WP-06h — Track B CUDA interop fill + frame-sync progress + Qt surface stub

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | CUDA kernel fill into exported interop texture (`interop_fill.cu`) with honest stub + tests; `FrameSyncPair` progress counters across render thread + job lane; regenerate `composite.frag.spv` when glslang available (document otherwise); Editor Qt `viewport.vk_surface_handle` → `ViewportSwapchainHandoff` stub (headless-safe); keep Lavapipe + `demo_hybrid_hud` green |
| **MT note** | Render thread signals/waits timelines; CUDA fill via `submit_cuda` on job lane |
| **Exit** | `fuse_cuda_interop` interop fill + frame-sync progress tests; `fuse_editor_host` Qt surface handoff command; `RhiContext` wires fill + frame sync on submit; headless CI unchanged |
| **Deps** | WP-06g |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06h |

---

### WP-06i — Track B Qt QVulkanInstance progress + CUDA/timeline load stubs

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `QVulkanInstance::createSurface` bootstrap with winId fallback (`viewport_vulkan_surface.*`); `ViewportSwapchainHandoff::qtRealSurface`; `stressFrameSyncUnderLoad` + `stressInteropFillUnderLoad`; Lavapipe ICD tests `RUN_SERIAL`; build hygiene — restore corrupted `test_profiler_assert.cpp`, fix `cue_preview` + deepen-trim fallout |
| **MT note** | UI posts winId; game thread consumes handoff; CUDA stress stubs remain job-lane safe |
| **Exit** | `fuse_editor_host` viewport Vulkan bootstrap test; `fuse_cuda_interop` load-stress tests; parallel `ctest -j` Lavapipe targets green; `fuse_cinematics` builds |
| **Deps** | WP-06h |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06i |

---

### WP-06j — Track B Lavapipe teardown hardening + Qt embed/timeline stress

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | GPU drain on `FrameManager`/`VulkanDevice`/`VulkanBootstrap`/`HybridRendererBootstrap` teardown; `fuse_hybrid_vulkan_presentable` rapid create/render/shutdown rerun; ICD lock post-test quiesce + `RUN_SERIAL` on all Lavapipe targets; `stressViewportVulkanBootstrapTeardown` + `stressFrameSyncTeardownCycle` (headless-safe); keep serial `ctest -j1` green for Vulkan targets |
| **MT note** | Teardown on render thread only; editor embed stress posts from UI, consumes on game thread |
| **Exit** | `fuse_hybrid_vulkan_presentable` 8-cycle rapid rerun; `fuse_editor_host` + `fuse_editor_runtime_embed` teardown stress; `fuse_cuda_interop` timeline teardown cycle; Lavapipe ICD tests stable under `ctest -j1` |
| **Deps** | WP-06i |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06j |

---

### WP-06k — Track B Lavapipe tune + composite SPIR-V regen + Qt viewport swapchain recreate stubs

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | ICD lock quiesce tune + viewport GPU drain on teardown; `FuseShaderSpirvRegen.cmake` configure-time `composite.frag.spv` regen when `glslangValidator` on runner; `viewport_swapchain_recreate.*` headless-safe `PresentPath` resize/recreate stubs toward U6 real display; keep serial `ctest -j1` green for Vulkan targets |
| **MT note** | Viewport resize posted from UI; swapchain recreate on game thread only |
| **Exit** | `fuse_editor_host` + `fuse_editor_runtime_embed` swapchain recreate tests; `fuse_regen_shader_fixtures` target when glslang present; Lavapipe ICD tests stable under `ctest -j1` |
| **Deps** | WP-06j |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06k |

---

### WP-06l — Track B spirv-val regen gate + consumed swapchain present + PlaceholderRenderer toggle

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `spirv-val` gate on `fuse_regen_shader_fixtures` when available; deepen viewport recreate toward consumed swapchain present (`presentViewportSwapchainFrame` after handoff); `HybridComposer::setSoftwarePlaceholderEnabled` scoped embed progress; keep serial `ctest -j1` green for Vulkan targets |
| **MT note** | Viewport resize posted from UI; recreate + present on game thread only |
| **Exit** | `fuse_editor_host` + `fuse_editor_runtime_embed` consumed-present-after-recreate tests; `fuse_hybrid_renderer_bootstrap` software-placeholder toggle test; Lavapipe ICD tests stable under `ctest -j1` |
| **Deps** | WP-06k |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06l |

---

### WP-06m — Track B Qt vkQueuePresentKHR gate + PlaceholderRenderer embed deepen + mobile stubs

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `FUSE_ENABLE_QT_PRESENT` gate for `vkQueuePresentKHR` when display+Qt Vulkan surface available; `viewport_present_gate.hpp` + `realPresentEligible()` unify GLFW/Qt present paths; deepen consumed viewport present diagnostics; `shouldDisableSoftwarePlaceholderForEmbed` when external swapchain wired; `cmake/FuseVulkanMobile.cmake` Android/MoltenVK stubs; keep serial `ctest -j1` green for Vulkan targets |
| **MT note** | Viewport present on game thread only; Qt surface posted from UI |
| **Exit** | `fuse_hybrid_vulkan_presentable` Qt present gate tests; `fuse_editor_host` viewport Qt present gate test; Lavapipe ICD tests stable under `ctest -j1` |
| **Deps** | WP-06l |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06m |

---

### WP-06n — Track B Qt present path readiness + timeline/CUDA combined stress

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `realQtPresentEligible()` + `viewportQtPresentPathReady()` toward display present (headless-safe defaults); deepen viewport present diagnostics (`qtPresentPathReadyTicks`, `qtRealPresentCallCount`); `stressFrameSyncAndInteropFillUnderLoad()` + iteration deepen (24-frame load, 6×16 teardown); fix `RuntimeEmbedSession::reset()` counter hygiene; keep serial `ctest -j1` green for Vulkan targets |
| **MT note** | Viewport present on game thread only; CUDA combined stress stays stub-safe on CI |
| **Exit** | `fuse_hybrid_vulkan_presentable` Qt present runtime tests; `fuse_editor_host` Qt present path readiness test; `fuse_cuda_interop` combined stress test; Lavapipe ICD tests stable under `ctest -j1` |
| **Deps** | WP-06m |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06n |

---

### WP-06o — Track B Qt present path eligible diagnostics + scoped PlaceholderRenderer retirement deepen

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `viewportQtPresentPathEligible` in `ViewportSwapchainPresentResult`; embed session `qtPresentPathEligibleTicks` + `softwarePlaceholderRetiredTicks`; deepen `shouldDisableSoftwarePlaceholderForEmbed` for path-ready retirement; `HybridComposer::softwarePlaceholderSkippedFrames`; consolidate present diagnostics helpers in `runtime_viewport.cpp`; keep serial `ctest -j1` green for Vulkan targets |
| **MT note** | Viewport present + placeholder retirement on game thread only |
| **Exit** | `fuse_editor_host` + `fuse_editor_runtime_embed` placeholder retirement tests; `fuse_hybrid_renderer_bootstrap` skipped-frame counter test; Lavapipe ICD tests stable under `ctest -j1` |
| **Deps** | WP-06n |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06o |

---

### WP-06p — Track B mobile Vulkan stubs deepen + PlaceholderRenderer retirement tests + Lavapipe ICD tune

| Field | Value |
|-------|-------|
| **Effort** | M |
| **Scope** | `mobile_vulkan_stub.hpp` + `FuseVulkanMobile.cmake` status helper; `test_viewport_present_gate` + `fuse_placeholder_renderer_retirement` tests; serial ICD lock on `fuse_rhi_resource_destroy_order`; ECS `migrate_entity` indexed column writes + mechanics `Component` destructor hygiene for umbrella green; keep serial `ctest -j1` green for Vulkan targets |
| **MT note** | Headless-safe stub tests only; no display required |
| **Exit** | `fuse_mobile_vulkan_stub` + `fuse_editor_viewport_present_gate` + `fuse_placeholder_renderer_retirement` tests; Lavapipe ICD targets stable under `ctest -j1`; ECS/mechanics segfaulting tests restored |
| **Deps** | WP-06o |
| **Status** | ✅ Landed — see [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §WP-06p |

---

### WP-07 — U5 Feature modules (parallel per module)

| Field | Value |
|-------|-------|
| **Effort** | XL (5 sub-packages) |
| **Sub-packages** | `fuse_ai`, `fuse_cinematics`, `fuse_fx`, `fuse_mechanics`, `fuse_adventure` — **each parallelizable** after WP-06 |
| **MT note** | AI/FX jobify per architecture §7 |
| **Exit** | Per-module U5 gates in prestarter §10 |
| **Deps** | WP-06 |
| **Status** | ✅ Prestarter §10 gates closed; post-gate ore through U5 wave 20 (nested composite codegen, FSEvents poll, timeline host, mission VM delay-ms, btDbvt bridge, combat hitscan/reload, conv state machine) — [U5-MODULES.md](./U5-MODULES.md) |

---

### WP-08 — U6 Qt editor vertical slice

| Field | Value |
|-------|-------|
| **Effort** | L |
| **Scope** | In-process PIE; UI thread vs game thread command queue; one feature pane |
| **Deps** | WP-06, WP-05 |
| **Status** | 🚧 Wave 10: Qt real-surface live present counters + project VFS mount on embed load — [U6-EDITOR.md](./U6-EDITOR.md) |

---

### WP-09 — U7 Project format + converters

| Field | Value |
|-------|-------|
| **Effort** | L |
| **Scope** | `project.json`, importers, cookers under `Tools/FUSE/` |
| **Deps** | WP-06 |
| **Status** | 🚧 Wave 12: async material cook-cache integration + editor embed drain — [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md) |

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
| **U6** | Editor PIE | UI/game thread queue + coalesced property undo + headless runtime embed — 🚧 Qt External GPU viewport remains |
| **U7** | Project format | `fuse_project` + wiring stubs + toybox convert + cook encoder hooks — 🚧 link real Assimp/BC7/OGG libs |
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

**Next:** U2 incremental — expand Engine probe + SimObject/StringTable route; U6 full Qt `vkQueuePresentKHR` on display with `FUSE_ENABLE_QT_PRESENT=ON`; U7 real ispc_texcomp + libvorbisenc on CI images; Track B post–WP-06p (driver-wired timeline stress on NVIDIA CI, full software placeholder removal).

**U6/U7 wave 13 landed:** `fuse_cook` cook-cache persist + skip re-cook progress; `ispc_texcomp` honest stub hook; vorbis WAV sniff deepen; Qt WSI instance/extension probe headless-safe.

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
