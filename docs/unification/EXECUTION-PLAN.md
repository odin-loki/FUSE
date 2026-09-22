# FUSE — Port Execution Plan (Track B, from B2.11)

**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) · **Headings:** [FUSE_MASTER_PLAN_TOC.md](../plans/FUSE_MASTER_PLAN_TOC.md)
**Vulkan detail:** [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) · **Roadmap:** [roadmap.md](../roadmap.md)

This is the working order for continuing the port. The rule from the master plan still holds:
*a phase's gates go green before the next phase builds on it* ("After **B2** gates are green …
begin **B3**"). Track A P0–P7 landed; B2 has deep scaffolding but its B2.11 gate list was
unverified. The next stage is to **prove B2** rather than add surface area.

---

## 0. Ground rules for every step

1. Build `fuse-debug` preset (Vulkan ON, Lavapipe in CI), run the full CTest suite.
2. Suite must be 100% green **and** stable under `ctest -j8` (repeat ×3 for new async code).
3. `fuse_vulkan_validation_gate` must pass: whole suite re-run under
   `VK_LAYER_KHRONOS_validation`, zero validation messages.
4. Stub backend (`-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON`) must still compile and pass.
5. Commit per completed step with the gate evidence in the message.

---

## 1. Baseline (done)

| Item | Result |
|------|--------|
| Baseline suite | 165/172 → **173/173** (incl. new gate tests), Debug + stub-backend Release |
| ECS `migrate_entity` | Use-after-free: `Archetype&` held across `m_archetypes` growth |
| WorldPartition / Terrain streaming queues | Jobs captured `this` with no lifetime guard (heap corruption); completion vs in-flight counter race |
| Raster depth | Missing `SAMPLED` usage → Lavapipe segfault on bindless registration |
| P7 parity test | Homebrew cooker wrote wire-less `.fuselevel` into the sample tree, breaking `demo_fx` when run first |

## 2. B2.11 gate: zero validation errors (done)

Ran every test with the Khronos validation layer: **28 distinct VUIDs** → **0**.

| Area | Fix |
|------|-----|
| Device | `VK_KHR_swapchain` only with instance `VK_KHR_surface`; enable storage-image / UBO update-after-bind and non-uniform indexing features the bindless layout and composite shader already use; bindless binding flags gated per type |
| Surface query | No `vkGetPhysicalDeviceSurfaceSupportKHR` without `VK_KHR_surface` |
| Frame ring | Real per-slot `fenceSubmitted` tracking — the old path treated `VK_NOT_READY` as "stub" and reset command pools still executing; fence-less transfer/compute aux submits drained on slot retire; fence reset before first direct submit |
| Timestamps | Query results read only after the queries were submitted |
| Teardown | `VulkanDevice::waitIdle()` before `RhiContext` members are destroyed |
| Descriptors | No null-handle clear writes (bindings are `PARTIALLY_BOUND`); buffers route to UBO vs SSBO heap by usage; GPU write skipped when usage bit missing |
| Layouts | Persistent tracked layouts for raster color/depth, swapchain images and the CUDA interop image; graph barriers use the tracked old layout; present barriers only touch the acquired swapchain image |
| Composite | Own offscreen target compatible with its pipeline's render pass (it previously rendered into the raster framebuffer it samples, with an incompatible render pass and no clear values); sampled sources transitioned to `SHADER_READ_ONLY_OPTIMAL` first |
| Deferred / shadows | `R32F` G-buffer depth and shadow maps are colour targets per B5.2/B5.5 (`R32F` cannot be a depth attachment or exported with that usage) |
| Gate | `fuse_vulkan_validation_gate` CTest (serial, skips without the layer); Linux CI installs `vulkan-validationlayers` |

## 2b. Test harness honesty (done)

`run_vulkan_icd_locked.sh` — the wrapper every ICD-using test runs through — **exited 0 on every
failure** (`if ! cmd; then status=$?` captures the negation, i.e. 0) and silently retried
segfaults. Around 20 Renderer/Hybrid/Editor tests could not fail. It is now
`exec flock -x <lock> "$@"`. Unmasking surfaced these, all fixed:

| Test | Root cause |
|------|-----------|
| `fuse_editor_host` | `HybridRendererBootstrap::shutdown` dereferenced a null `rhiContext()` after a failed init |
| `fuse_editor_host`, `fuse_editor_runtime_embed` | Present/retirement checks relied on Lavapipe accepting a fake `VkSurfaceKHR`; an External surface now requires instance `VK_KHR_surface` (else headless), and checks are gated on a wired swapchain |
| `fuse_editor_viewport_present_gate` | Contradictory expectations; the software placeholder now stays until a swapchain is wired |
| `fuse_editor_runtime_embed` | Embed worlds were never attached when the world loaded before the hybrid renderer existed; test also needed the repo root as working directory |
| `fuse_hybrid_module_gates_tests` | Lever volume ignored the VActor mount offset; combat loop fired from an empty single-round magazine; mount-depth and conversation-branch checks read state later overwritten by timeline cues / follow-up dispatches |
| `fuse_core_new_ban` (Release) | Asserted a Debug-only guard in every configuration |

---

## 3. Next — remaining B2.11 gates that CI can prove (ordered)

Each gets a dedicated test; hardware-only gates (RTX 3090, Nsight, 60 fps, Win32 handles,
RenderDoc captures) are tracked in §5 and are **not** faked.

| # | Gate (master plan wording, abbreviated) | Test to add | Notes |
|---|------------------------------------------|-------------|-------|
| 3.1 ✅ | Triangle on screen — white triangle, black background, correct winding, no validation errors (**Week 1 gate**) | `fuse_b2_triangle_readback` | Done: `RasterPath::readbackColor`; corners black, apex/base white, orientation asymmetry check, coverage 0.1250 (expected 0.125), pixel-identical second frame; `minimal.frag` is white |
| 3.2 ✅ | Render graph compiles in < 1 ms CPU per frame | `fuse_b2_render_graph_budget` | Done. Release median: hybrid frame 0.69 µs, worst-case 32-pass live chain (54 barriers) 9.8 µs. Debug enforces a 10 ms gross-regression bound (164 µs measured) |
| 3.3 ✅ | Staging ring wraps — 256 MB in 1 MB chunks, no corruption | `fuse_b2_staging_wrap` | Done: 16 MB ring, 256 live 1 MB device-local buffers, 15 wraps, all verified after the last upload (< 1 s on Lavapipe) |
| 3.4 ◐ | Async upload completes and signals fence — fence wait timeout test | `fuse_b2_staging_wrap` | Every upload signals its fence within the 1 s bounded wait (256/256, 0 timeouts). **Open:** uploads are still synchronous (wait per copy); a truly async upload queue with deferred ring-slot reuse is follow-up work |
| 3.5 ◐ | Zero per-frame heap allocations — frame memory from per-frame LinearAllocator | `fuse_b2_frame_alloc_budget` | `RhiContext` render thread: 27 → **0** allocations/frame over 64 steady-state frames (render graph uses pooled accesses + member scratch; submit result message is static). **Open:** `HybridComposer::render` path and job-worker threads not yet measured |
| 3.6 ✅ | Composite blends CUDA and raster output at all GRIA α values | `fuse_b2_composite_blend` | Done: composite target read back at α ∈ {0, .25, .5, .75, 1}; background and triangle pixels equal `mix(cuda, raster, α)` within ±2/255 (CUDA source is the shader constant without a toolkit; a live CUDA source falls back to a monotonicity check) |
| 3.7 ✅ | Bindless register/unregister — no descriptor heap corruption | `fuse_b2_bindless_churn` | Done. Found real corruption: the CPU heap handed out texture/buffer slots up to 65536 while the Vulkan arrays hold 1024, so slot ≥ 1024 wrote outside the set. Slots are now capped at `kBindlessGpuArrayCapacity` once the Vulkan set exists (1500 requests → 1024 live, 476 rejected cleanly); 3000-cycle churn recycles inside the array with no leaks. **Follow-up:** size the arrays from device descriptor-indexing limits instead of 1024 |
| 3.8 → §5 | All Vulkan objects named via `vkSetDebugUtilsObjectNameEXT` | manual (RenderDoc) | Vulkan cannot read names back, so CI can only spot-check names that appear in validation messages (frame ring, raster targets are named). Full audit stays a RenderDoc capture per the master plan |
| 3.9 → §5 | Swapchain 1920×1080 triple-buffered, resize rebuilds without crash | manual / Xvfb follow-up | Headless recreate path is covered by existing present-path tests; a real `VkSwapchainKHR` needs a display server (possible CI follow-up: Xvfb + GLFW window backend) |

## 4. Then — B3 (ECS, spatial, scene) on the verified B2 base

Order follows the master plan: B3.1 ECS → B3.2 components → B3.3 systems → B3.4 BVH →
B3.5 SVO → B3.6 scene manager → B3.7 serialisation → B3.8 camera → B3.9 gates.
Scaffolding exists for most of these; each sub-phase starts by running its B3.9 gate rows
against today's code and only then deepening (same pattern as §2).

| Gate rows | Test | Status |
|-----------|------|--------|
| ECS: 1M entities + stale generations; archetype grouping; exact `each`; `each_parallel` == `each` over 100 randomised cases; migration preserves data | `fuse_b3_ecs_gates` | ✅ (plus the `migrate_entity` UAF fixed in §1) |
| ECS: 100k Transform+Mesh+RigidBody > 500M components/s single-threaded | `fuse_b3_ecs_gates` | ◐ Column lookup hoisted out of the row loop: Release 60M → ~225–290M/s here. Components total 340 B/entity, so this runner is memory-bound (~15 GB/s); CI enforces a 100M floor, the 500M baseline is a §5 workstation measurement |
| BVH: SAH build 100k < 500 ms; 100k rays vs 10k objects == brute force; frustum == brute force; refit after 1k updates; frustum cull 10k < 0.1 ms | `fuse_b3_bvh_gates` | ✅ The old SAH build was O(n²) per node (every split re-merged the range) — replaced with 16-bin SAH: 55 ms Release. Added `update_leaf_aabb` (the refit row had no way to move a leaf). 0/100k ray mismatches; cull 0.064 ms |
| SVO, scene, camera rows | — | Next: `fuse_b3_svo_gates`, then scene build/cull/serialise |

## 5. Hardware / manual gates (not provable in CI)

RTX 3090 device selection, Nsight occupancy, ECS 500M components/s, 60 fps / < 8 ms at 1080p, Win32 external memory
handles, `cudaImportExternalMemory`, `compute-sanitizer`, RenderDoc captures, and on-screen WSI
present. These need a developer machine with the target GPU; record results in
[TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) when run.
