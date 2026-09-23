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
| Baseline suite | 165/172 → **173/173** (incl. new gate tests), Debug + stub-backend Release; now 191/191 Debug, 190/190 stub Release with the B3/B4 gates |
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
| 3.4 ✅ | Async upload completes and signals fence — fence wait timeout test | `fuse_b2_staging_wrap`, `fuse_b2_async_upload`, `fuse_b2_upload_queue_family` | `UploadQueue`: copies batched with fences, ring space retired in submission order only after its fence (96 uploads, up to 7 batches in flight, 33/33 tickets, 0 timeouts, data intact after the caller reuses its memory); sync validation clean. Cross-family path tested: test layer `VK_LAYER_FUSE_split_transfer_family` (below validation) gives Lavapipe a transfer-only family 1; buffers + 7-mip x 3-layer image, duplicates per batch, partial re-upload of live data, readback on graphics without CPU wait; 0 validation messages (incl. syncval), negative controls (orphan acquire, cross-queue race) reported |
| 3.5 ✅ | Zero per-frame heap allocations — frame memory from per-frame LinearAllocator | `fuse_b2_frame_alloc_budget` | `RhiContext` render thread: 27 → **0** allocations/frame over 64 steady-state frames (render graph uses pooled accesses + member scratch; submit result message is static). `HybridComposer::render`: 0 allocations/frame, enforced by `fuse_b2_hybrid_alloc_budget`. **Open:** World2D/3D `tick()` still allocates 7/frame |
| 3.6 ✅ | Composite blends CUDA and raster output at all GRIA α values | `fuse_b2_composite_blend` | Done: composite target read back at α ∈ {0, .25, .5, .75, 1}; background and triangle pixels equal `mix(cuda, raster, α)` within ±2/255 (CUDA source is the shader constant without a toolkit; a live CUDA source falls back to a monotonicity check) |
| 3.7 ✅ | Bindless register/unregister — no descriptor heap corruption | `fuse_b2_bindless_churn` | Done. Found real corruption: the CPU heap handed out texture/buffer slots up to 65536 while the Vulkan arrays hold 1024, so slot ≥ 1024 wrote outside the set. Slots are now capped at `kBindlessGpuArrayCapacity` once the Vulkan set exists (1500 requests → 1024 live, 476 rejected cleanly); 3000-cycle churn recycles inside the array with no leaks. Arrays are now sized from the device's update-after-bind limits (Lavapipe 65536/16384/65536/16384) and the CPU caps follow; storage-only textures route by usage |
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
| SVO: 1M voxels at depth 10 round-trip; ray == brute force (10k rays); carve transitions; SDF continuity | `fuse_b3_svo_gates` | ✅ Replaced the scaffold (linear-scan `get`/`set` — 1M inserts were O(n²) — half-voxel ray stepping, and a carve that *added* material) with octree walks, a 3D-DDA ray cast, CSG-subtraction carve and trilinear SDF. 0 errors / 0 of 10k ray mismatches / 0 carve state or sign errors / max 0.01 jump per 0.01 step. Brick leaves (8³) now: 1M scattered voxels 195 MB → 32 MB, 256³ fill 1.2 MB |
| Scene: 1000 mesh+SDF build < 1 ms; cull draw counts == brute force; mid-frame add/remove/move; 10k transform update < 1 ms; full build < 2 ms | `fuse_b3_scene_gates` | ✅ SceneManager now runs Transform/Camera systems and keeps a `spatial::BVH` of every Mesh/SDF (refit when the set is unchanged, rebuild on spawn/despawn) and `buildFrame()` culls through it. Release: first build 0.63 ms, steady 0.15 ms, transform 10k ~0.4 ms. Fixed on the way: O(roots·n) hierarchy walk (53 ms → 0.3 ms serial), a job-system lost-wakeup that added ~1 ms to every `parallel_for`, a latent `parallel_for`-in-job deadlock it exposed, 2-corner mesh bounds (wrong under rotation) and the O(n²) cull fallback |
| Serialisation: 10k entities save/load byte-identical; async load callback on the main thread | `fuse_b3_serialiser` | ✅ New `RegistrySerialiser` ('FECS' v1: record table + archetype blocks keyed by `component_name`) and `RegistryLoadQueue` (parse on a worker, apply + callback in `pump()`). 10,001 entities round-trip with identical ids and bytes; corrupt/truncated files rejected; ASan/UBSan clean |
| Remaining B3 rows | — | CUDA managed-memory column read, SDF buffer → ray marcher, 500 SDF @ 1080p > 60 fps, RenderDoc ordering, SVO 1M rays on CUDA: GPU/workstation (§5) |

## 4b. Then — B4 (physics) on the verified B3 base

Same method: run the B4.11 rows against the existing code first. Most of B4 was scaffolding with
guard/preflight helpers around stubs (a PhysicsManager that fabricated bodies, a GJK that fell
back to an AABB test, an `Svo` that only counted carves, cloth without velocity update); each
row below replaced the stub it exercised. All tests are `fuse_physics` executables; timing rows
are enforced in optimised builds and marked `RUN_SERIAL` + `perf;gate`.

| Gate rows | Test | Status |
|-----------|------|--------|
| Spatial hash finds every overlapping pair of 10k random spheres (vs O(n²)); no missed pairs for grid-aligned / multi-cell bodies | `fuse_b4_broadphase_gates` | ✅ 653 overlaps, 0 missed; refine now uses the insertion bounds (boxes were refined as spheres of radius `halfExtents.x`); planes paired only when a body shared the plane body's cell — fixed |
| Sphere-sphere vs analytic (Bullet) within 0.001; sphere-plane at all angles; capsule-capsule parallel + degenerate | `fuse_b4_narrowphase_gates` | ✅ exact to float; capsule-capsule added (Ericson segment closest points) |
| GJK vs SAT on 10k convex pairs; EPA depth within 0.01 | `fuse_b4_narrowphase_gates` | ✅ distance GJK + EPA written (0 mismatches over 10k oriented boxes, worst EPA depth error 8e-7, ≤ 10 GJK / 16 EPA iterations) |
| SDF collision normals smooth across surface transitions | `fuse_b4_narrowphase_gates` | ✅ `collideSphereSdf` (gradient normal): 0.19° per 1 mm step on a smooth union vs 44° on the hard-union control |
| Free fall hits ground at √(2h/g); stack of 10 stable 5 s; restitution 1.0 equal rebound; friction stops a sliding box at v²/(2μg); distance constraint ± 0.01; sleep | `fuse_b4_solver_gates` | ✅ 1.433 s vs 1.428 s; rebound 1.99 / 2.00 m; box slides 2.553 m vs 2.548 m. Added the XPBD velocity pass (restitution was never read), static friction on substep displacement (the old term used the offset between body centres), box-plane contacts |
| 100 m/s sphere vs 0.1 m wall: discrete misses, CCD catches; CCD < 1 ms for 100 fast bodies | `fuse_b4_ccd_gates` | ✅ 50/50 shots caught, 0.49 ms. CCD now clamps in the solver (the manager's sweep ran an unswept broadphase and discarded its TOIs). TOI binary-search row: N/A — sweeps are closed form |
| Cloth 32×32 stable at 1/60; pins exact; wind direction; cloth-sphere without interpenetration; 64×64 < 1 ms | `fuse_b4_cloth_gates` | ✅ XPBD cloth rewritten (shear/bend, long-range tethers, per-particle drag, sphere contact with friction, banded parallel solve): stretch ≤ 1.05, 0 penetration, 64×64 ≈ 0.7 ms |
| Transforms reflect physics every frame; `apply_impulse` Δv = J/m; Enter/Exit without misses or spurious callbacks; kinematic path + push; 1000 active < 8 ms; full pipeline 1000 bodies < 4 ms; 10k sleeping < 0.5 ms | `fuse_b4_manager_gates` | ✅ real ECS bridge (`Collider` component, `TagKinematic`). Solver fixes: centre-based `minSeparation` for all shapes (boxes sank into each other), forces over all substeps, kinematic motion, per-constraint delta application that walked every body and raced across island jobs. Perf: 1000 bodies 1.6 ms step / 0.65 ms single pass, 10k sleeping 0.016 ms |
| Sphere carve exact; dual contouring watertight; debris mass ∝ voxels; debris collides; 10×5 debris no spike > 10 ms; 5×10 debris < 16 ms | `fuse_b4_destruction_gates` | ✅ `VoxelVolume` (carve, dual contouring, floating-piece detach) + ECS debris spawn: 590/590 voxels, 0 open edges, debris frames ≈ 0.9 ms |
| GPU radix sort; broadphase < 2 ms / 10k (RTX 3090); narrowphase < 3 ms / 1k (RTX); solver 10 iterations × 10k contacts < 5 ms (RTX) | — | §5 (CUDA). CPU references: 1k-pair narrowphase 0.24 ms, 1000-body single pass 0.65 ms |

**Rotation (done, `fuse_b4_rotation_gates`):** inertia, orientation integration, per-point contacts with
generalized inverse mass, oriented box/capsule narrowphase, off-centre impulses; edge/corner drops settle flat,
incline slide 2.227 m vs 2.219 m. **Follow-ups:** distance constraints ignore local anchors; CCD sweeps position only; the destructible volume
has no collision shape of its own yet; `Scene::SVO` and the physics `VoxelVolume` are separate.

## 4c. B1 re-audit, B5, B6, B7 — parallel work streams

Run as independent streams (one owner per module / Renderer CMake fragment, builds serialised on
one lock), each committed after its gate test passed in Debug and stub Release; the combined HEAD
was re-verified from a clean checkout. Every stream found real bugs in the scaffolding it touched.

| Phase / area | Gate test(s) | Result |
|--------------|--------------|--------|
| B1 Core | `fuse_core_b1_{jobs,memory,math,logging,platform}_gates`, `fuse_core_b1_assert_codegen` | Job-system deadlock (a waiting job blocked its worker's only fiber — now per-worker fiber pools), `JobCounter` races (TSan clean), fiber start bug, 16-byte arenas misaligning 64-byte requests, pool double free, truncated trace JSON, raw-mouse mixing. 1M jobs, 10k random DAGs, allocators 1M ops. Not met: engine-wide zero heap use; profiler 10 ns needs reference hardware |
| B5.2/5.3 G-buffer, PBR | `fuse_b5_gbuffer_materials_gates` | Octahedral decode wrong for z<0 (CPU and DDGI); signed oct storage (0.00085 rad through RGBA16F; unsigned form failed at 0.00205); GLSL BRDF α vs α², double 4NoVNoL; material row layout and never-uploaded SSBO fixed; procedural wood/metal/concrete; 1000-material fetch exact |
| B5.4 Clustered | `fuse_b5_clustered_gates` | Cluster AABBs non-conservative / ignored camera rotation+FOV; culling == brute force (0/3456); CPU deferred shade bit-identical to all-lights |
| B5.5 Shadows | `fuse_b5_shadows_gates`, `fuse_csm_guards` | Shimmer-free bounding-sphere CSM (0 frame-diff px vs 24); vertical sun NaN fixed; SDF soft shadows within 2.97% of a path tracer |
| B5.6 DDGI | `fuse_b5_ddgi_gates`, `fuse_b5_ddgi_timing` | Real CPU trace/blend/sample; 0.96% vs Monte Carlo; emissive 0.85% vs form factor; change detection needed for the 64-frame response (plain 0.97 hysteresis reaches 5.9%) |
| B5.7/5.9 TAA, HBAO, SSR | `fuse_b5_taa_ssfx_gates` | CPU TAA resolve (edge error 0.294 → 0.054, ghost residue 0.001 at 30 m/s); HBAO 0.500 at a 90° corner; SSR 98.2% found, 0.85% colour error |
| B5.8/5.11 Sky, fog | `fuse_b5_atmosphere_gates` | Heuristic sky → single scattering (5.9% vs brute force); inverted sky-LUT axis; sun disk 0.5°; 10-bit dithered output; fog vs closed form |
| B5.10 Post | `fuse_b5_post_gates` | Bloom knee discontinuity and no spread; DoF, motion blur new; ACES mid-grey calibration (−0.468 EV); per-pixel grain; auto-exposure sign |
| B6 Editor (Qt-free) | `fuse_editor_b6_{command,hierarchy,gizmo,panels,play_mode}_gates` | 14 bugs (redo invalidation, delete undo, gizmo space/snap/picking, reparent cycles, play-mode restore). Visual rows manual (no Qt in CI) |
| B7.1 Animation | `fuse_b7_animation_gates` | TRS decompose lost rotations; FABRIK wasn't FABRIK; IK/blends ignored rotations; clip/skeleton files. 6/8 rows (GPU skinning hardware) |
| B7.2 Audio | `fuse_audio_b7_gates`, `fuse_audio_b7_perf` | dt-driven play heads (crackle), pitch scaled amplitude, linear curve was log, reverb mono; CPU reverb −132 dB vs reference |
| B7.3 Script | `fuse_script_b7_gates`, `fuse_script_b7_perf` | Lua had never run (no system Lua → null stub); bundled Lua 5.2 + bindings, runtime, hot reload, sandbox, budgets |
| B7.4 Net | `fuse_b7_net_gates` | ENet backend was a stub; rollback/delta/interpolation exactness bugs; 2-process reliable transfer through a 10%-loss proxy |
| B7.5/7.6 Terrain, streaming | `fuse_b7_terrain_gates`, `fuse_b7_streaming_gates` | White-noise terrain, LOD bounds, deform; streaming budgets ignored in-flight loads, eviction loops; 0 pop-in over 2 km at 30 m/s |
| B7.7/7.9 VFX, assets | `fuse_b7_vfx_gates`, `fuse_b7_cook_gates` | Collision never implemented, emission drift; BC7 encoder invalid, mesh cook wrote text, cook cache never persisted |
| B2.11 / B3 follow-ups | see §3 rows 3.4/3.5/3.7 and §4 SVO row | Async uploads, device-sized bindless, composer 0 allocs; SVO bricks |

| B7.8 Platform | `fuse_core_b7_platform_gates`, `fuse_core_b7_shipping_strip` | Crash handler was a flag-only stub → POSIX signal reports (SEGV/ABRT/FPE/stack overflow, chaining); no leak detector existed → debug detector, 0 leaks over 150 init/shutdown cycles; shipping kept all log/profiler code → `FUSE_NO_LOGGING`/`FUSE_NO_PROFILER` |
| B1 jobs | `fuse_core_b1_parallel_for_alloc_gates` | `parallel_for` allocated ~17 per call → 0 (flat, nested, in-job); median 4096/256 at 4 workers 54 µs → ~1–4 µs |
| B4 follow-ups | `fuse_b4_joint_gates`, `fuse_b4_ccd_gates`, `fuse_b4_rotation_gates` | Joint anchors were ignored; pendulum periods within 0.04%; rotational CCD (spinning bar stopped at post); twisted 8/12-box stacks sleep; cloth 64×64 ~0.65 → ~0.4 ms |
| Batch 3 | see commits | Compute SSAO/SSR/SSGI via shared `fuse_ssfx`; ECS `create_at` + SDF CSG; editor Play drives `PhysicsManager`; renderer TAA/DDGI integration with 0 sync-validation hazards |

| Batch 4 | `fuse_b4_pipeline_alloc_gate`, `fuse_b4_joint_gates`, `fuse_b2_upload_queue_family`, `fuse_core_b1_log_async_ring_gates`, `fuse_core_b1_x11_window_gates`, `fuse_b2_x11_swapchain_gates`, `fuse_b5_rhi_*`, `fuse_editor_b6_viewport*_gates`, `fuse_editor_b6_material_profiler_gates`, `fuse_scene_magic_gates`, `fuse_b7_save_reload_gates`, `fuse_b3_free_camera_gates`, `fuse_runtime_steady_state_alloc`, `ctest -L lint`, `ctest -L valgrind`, `fuse-asan` preset | Physics worlds 18.8k → 0 allocs/frame; joint API with XPBD limits and break events; cross-queue-family uploads fixed (test layer splits Lavapipe's family); lock-free async log ring; native X11 window + real swapchain under Xvfb; ENet/Lua vendored; strict asset import by default; draw lists reached the GPU for the first time; ASan+UBSan over every FUSE target |

**Open items:** CUDA kernels are stubs; lavapipe allocates inside `vkCmd*` (engine code itself is 0/frame);
Windows crash minidump and DPI awareness untested (no Windows toolchain); the editor presents headless
until `RhiContext` can adopt the `QVulkanInstance`.

## 5. Hardware / manual gates (not provable in CI)

RTX 3090 device selection, Nsight occupancy, ECS 500M components/s, CUDA physics timings (radix sort, 10k broadphase, 10k-contact solver), 60 fps / < 8 ms at 1080p, Win32 external memory
handles, `cudaImportExternalMemory`, `compute-sanitizer`, RenderDoc captures, and on-screen WSI
present. These need a developer machine with the target GPU; record results in
[TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) when run.
