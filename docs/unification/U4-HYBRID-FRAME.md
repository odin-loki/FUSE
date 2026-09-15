# U4 / WP-06 — Dimension APIs & Hybrid Frame

**Phase:** U4 (WP-06)  
**Date:** 2026-09-15  
**Status:** Frame barrier + JobScheduler cull orchestration deepened — software placeholder renderer; real GL/Vulkan deferred to Track B

---

## Delivered (this PR)

| Component | Location | Notes |
|-----------|----------|-------|
| `FrameCtx` / `FrameBarrier` | `Source/FUSE/Core/include/fuse/frame/` | Per-frame timing + tick sync hook |
| `IDimension` / `WorldHandle` | `Source/FUSE/Core/include/fuse/dimension/` | Stable dimension API surface |
| `World2D` | `Source/FUSE/World2D/` | Snapshot build + `parallel_for` cull stub |
| `World3D` | `Source/FUSE/World3D/` | Snapshot build + `parallel_for` cull stub |
| `HybridComposer` | `Source/FUSE/Hybrid/` | 3D clear → 2D sprites → UI overlay order |
| `PlaceholderRenderer` | `Source/FUSE/Hybrid/` | 320×240 RGBA software buffer (honest stub) |
| `demo_hybrid_hud` | `Source/FUSE/Apps/HybridHud/` | 60-frame spin demo |
| Tests | `fuse_hybrid_tests`, `fuse_world2d_tests`, `fuse_core_frame_barrier_tests` | MT snapshot cull, compositor flags, `FrameBarrier` unit + hybrid integration |

---

## Frame pipeline (v1)

```
game thread:
  1. HybridComposer::tick(ctx)
       - FrameBarrier::beginTick(frameIndex)
       - World3D::tickGameThread  → physics + immutable snapshot (serial)
       - World2D::tickGameThread  → physics + immutable snapshot (serial)
       - JobScheduler::submit per enabled dimension:
           World3D::runParallelCull  → internal parallel_for over snapshot
           World2D::runParallelCull  → internal parallel_for over snapshot
       - JobCounter::wait()  (dimension culls may overlap)
       - FrameBarrier::signalTickJobsComplete()
  2. HybridComposer::render(ctx)  [render thread only]
       - 3D clear colour (PlaceholderRenderer)
       - 2D sprite draws from immutable snapshot
       - UI overlay stub
```

**Threading rules enforced:**
- Scene graph mutation stays on the game thread (`World2D::addSprite`, `tickGameThread`, etc.).
- `HybridComposer::tick` forks dimension cull jobs via `JobScheduler::submit`; each world's `runParallelCull` uses `jobs::parallel_for` over its snapshot only.
- Workers read `SceneSnapshot2D` / `SceneSnapshot3D` only — no raw `SceneObject*` in `parallel_for` bodies.
- `FrameBarrier::signalTickJobsComplete()` runs only after the outer `JobCounter` join (all dimension culls finished).
- GPU touch gated by `fuse::platform::isRenderThread()` (registered at `fuse::core::initialize()`).

---

## Project dimension flags

```cpp
fuse::hybrid::DimensionFlags flags;
flags.enable3D = true;
flags.enable2D = true;
flags.enableUI = true;
composer.setProjectFlags(flags);
```

Maps to future `project.json` dimension toggles.

---

## Build & run

```bash
cmake -B build -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_HYBRID_DEMO=ON \
  -DFUSE_BUILD_LEGACY=ON \
  -DFUSE_BUILD_SMOKE=ON \
  -DFUSE_BUILD_T3D=OFF -DFUSE_BUILD_T2D=OFF
cmake --build build
ctest --test-dir build
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

---

## Gate U4 checklist

| Item | Status |
|------|--------|
| `IDimension` / `World2D` / `World3D` / `HybridComposer` headers stable for modules | ✅ |
| Demo: empty 3D clear + spinning 2D sprite in one process | ✅ (`demo_hybrid_hud`, software renderer) |
| Dimension enable/disable from project flags | ✅ |
| `parallel_for` cull stub (per-world + composer JobScheduler fork) | ✅ |
| Frame barrier between tick and render | ✅ (`fuse_core_frame_barrier_tests`, hybrid integration) |
| `renderThread()` ownership honoured | ✅ |
| No cross-thread raw `SceneObject*` in cull path | ✅ (tests) |
| TSan clean on cull path | ⏳ `fuse-tsan-nightly` workflow (cull + barrier tests included) |
| Real GLES/Vulkan present to window/swapchain | ❌ Track B RHI |
| Legacy T3D/T2D gfx backends wired | ❌ strangler phase |
| HDR / tonemap shared pass | ❌ Track B |
| Depth buffer + 3D mesh draw | ❌ placeholder clear only |
| Texture upload from jobs | ❌ `RenderUploadCommand` queue TBD |
| iOS/Android hybrid demo on device | ⏳ Android compiles `fuse_hybrid`; device run TBD |

---

## What still needs real GL/Vulkan

1. **Swapchain / surface** — window creation, resize, Android/iOS surface loss.
2. **RHI abstraction** — command lists, pipeline state, descriptor sets (Track B).
3. **3D mesh draw** — currently only clear-colour; no depth prepass.
4. **2D texture atlas** — sprite placeholder is flat-colour rotated quad.
5. **Hybrid compose** — GPU blit from 3D colour+depth target to 2D overlay target.
6. **Async upload lane** — staging buffers filled by jobs, committed on render thread.
7. **Editor PIE viewport** — Qt GL widget integration (U6).

---

## Related docs

- [architecture-parallel.md](./architecture-parallel.md) §4 — frame order
- [merge-strategy-2d-extends.md](./merge-strategy-2d-extends.md) §2 — `IDimension` API
- [work-plan.md](./work-plan.md) — WP-06
