# Track B — Deferred Rendering (B5.1–B5.12)

**Status:** B5.1 deferred frame pipeline + B5.2 G-buffer + B5.3 PBR materials + B5.4 clustered shading + B5.5 shadows + B5.6 DDGI + B5.7 screen-space effects + B5.8 atmosphere/sky + B5.9 TAA + B5.10 post-process stack + B5.11 volumetric/lighting stubs + **B5.12 Phase 5 integration gate**  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B5.1–B5.12  
**Phase source:** [P5.md](../sources/P5.md) §5.1–5.12  
**Depends on:** [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) B2.1–B2.11 (RHI bootstrap, render graph, CUDA job lane)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `DeferredFramePipeline` | `include/fuse/renderer/deferred/frame_pipeline.hpp` | 19-pass Phase 5 schedule (P5 §5.1 + B5.11 hooks) |
| `DeferredRenderer` | `include/fuse/renderer/deferred/deferred_renderer.hpp` | Owns G-buffer, material table, cluster culler, frame graph |
| `GBuffer` / `GBufferLayout` | `include/fuse/renderer/deferred/gbuffer.hpp` | Six attachments — octahedral normals, velocity, reversed-Z depth |
| `MaterialSystem` | `include/fuse/renderer/material/` | Bindless SSBO material table scaffold |
| `ClusteredLightCuller` | `include/fuse/renderer/lighting/clustered.hpp` | CPU cluster grid + sphere cull stub |
| `DirectionalShadow` / `ShadowPass` | `include/fuse/renderer/shadow/` | CSM atlas + render-graph shadow pass |
| `DDGI` | `include/fuse/renderer/gi/ddgi.hpp` | Probe grid, irradiance cache, update/sample stubs |
| `ScreenSpaceEffects` | `Source/FUSE/Compute/` | SSAO/SSR/SSGI via `submit_cuda` — see [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) |
| `SkyPass` / `SkyLut` | `include/fuse/renderer/atmosphere/` | Rayleigh/Mie CPU reference + LUT build |
| `TaaPass` / `TaaHistoryBuffer` | `include/fuse/renderer/taa/` | Halton jitter, ping-pong history, resolve facade |

**Not in scope (follow-up):** Real `vkCmd*` G-buffer draws, CUDA deferred-shade kernels, bindless descriptor pool, GPU post-process shader chain, full volumetric fog CUDA kernels, RenderDoc visual gates, RTX 3090 perf baselines.

---

## B5.1 — Rendering Architecture Upgrade

**Status:** `DeferredRenderer` + `DeferredFramePipeline` landed; 19-pass schedule populates `RenderGraph`.

| Component | Location | Notes |
|-----------|----------|-------|
| `DeferredPassId` | `deferred/frame_pipeline.hpp` | Depth prepass → UI composite/present |
| `DeferredFramePipeline::buildGraph` | `deferred/frame_pipeline.cpp` | Imports G-buffer textures, schedules Vulkan + CUDA passes |
| `DeferredRenderer::executeFrame` | `deferred/deferred_renderer.cpp` | Build + compile + execute per frame |

Pass schedule (19 total — 12 Vulkan, 7 CUDA):

1. `depth_prepass` — Vulkan
2. `gbuffer` — Vulkan
3. `shadow_maps` — Vulkan
4. `vk_to_cuda_signal` — Vulkan
5. `sdf_ray_march` — CUDA
6. `ddgi_probe_update` — CUDA
7. `clustered_light_cull` — CUDA
8. `deferred_shading` — CUDA
9. `sdf_shadows` — CUDA
10. `screen_space_ao` — CUDA
11. `volumetric_fog` — CUDA (B5.11)
12. `cuda_to_vk_signal` — Vulkan
13. `transparent_pass` — Vulkan
14. `atmosphere_sky` — Vulkan
15. `light_shafts` — Vulkan (B5.11)
16. `taa_resolve` — Vulkan
17. `post_process_stack` — Vulkan (B5.10)
18. `lens_flare` — Vulkan (B5.11)
19. `ui_composite_present` — Vulkan

---

## B5.2 — G-Buffer Layout

**Status:** Six-attachment layout + octahedral normal encoding + **CPU MRT pack/unpack validation** landed.

| Attachment | Format | Role |
|------------|--------|------|
| RT0 Normal+AO | RGBA16F | Octahedral-encoded normals |
| RT1 Albedo+α | RGBA8 | Base colour + opacity |
| RT2 Rough/Metal/Emissive/Shading | RGBA8 | PBR packed channels |
| RT3 Velocity | RG16F | Motion vectors for TAA |
| RT4 Depth | R32F | Reversed-Z depth |
| RT5 Emissive | RGBA16F | Emissive radiance |

| Component | Location | Notes |
|-----------|----------|-------|
| `GBufferLayout::channelCount` | `deferred/gbuffer.hpp` | Per-attachment channel validation (CPU) |
| `GBufferLayout::validateAttachmentFormats` | `deferred/gbuffer.cpp` | All slots resolve to non-`Undefined` formats |
| `GBufferPacking` | `deferred/gbuffer.hpp` | `pack` / `unpack` mirror `shaders/common/gbuffer.glsl` |

| Test | Validates |
|------|-----------|
| `fuse_gbuffer` | Formats, channel counts, layout validation, octahedral round-trip < 0.001 angular error, MRT pack round-trip, allocation |

---

## B5.3 — PBR Material System

**Status:** `Material` POD + `MaterialSystem` bindless SSBO scaffold + **parameter block extension rows** landed.

| Component | Notes |
|-----------|-------|
| `ShadingModel` | Opaque, Translucent, Emissive, SubsurfaceSSS, ClearCoat, Cloth |
| `MaterialParameterBlock` | `SubsurfaceParams`, `ClearCoatParams`, `ClothParams` authoring stubs |
| `MaterialFlagBits` | Procedural + bindless map presence (normal/AO/metallic) |
| `Material::pack()` | GPU SSBO layout — core PBR row + `subsurfaceBlock` / `clearCoatBlock` / `clothBlock` |
| `MaterialLayout::validateGpuStruct` | SSBO stride / offset checks for CPU tests |
| `MaterialSystem::registerMaterial` / `flushGpuBuffer` | CPU table → GPU buffer upload stub |

| Test | Validates |
|------|-----------|
| `fuse_material_system` | Pack extension blocks, texture flags, GPU layout validation, register/flush, SSBO allocation |

---

## B5.4 — Clustered Deferred Shading

**Status:** CPU cluster grid SoA + `ClusteredLightCuller` stub landed; tile/cluster index helpers, depth-slice mapping, light-grid rebuild overflow clamp, and assignment stats deepened (B5.4 follow-up); CUDA kernels deferred.

| Component | Location | Notes |
|-----------|----------|-------|
| `ClusterDesc` / `ClusterAABB` / `ClusterGridSoA` | `lighting/clustered.hpp` | 3D screen cluster grid types |
| `ClusterGridLayout` | `lighting/clustered.hpp` | Tile/cluster index encode/decode, bounds clamp, screen-depth → cluster index |
| `ClusterSliceLayout` | `lighting/clustered.hpp` | Exponential depth-slice near/far bounds + `computeSliceZFromDepth` (mirrors froxel layout) |
| `ClusterLightGridLayout` | `lighting/clustered.hpp` | Flat light-list packing, per-cluster capacity clamp, contiguous offset validation |
| `ClusteredLightCuller` | `lighting/clustered_light_culler.cpp` | CPU stub — builds cluster AABBs, sphere-culls point/spot lights, overflow stats, `rebuildLightGrid()` packs offsets |
| `DeferredFramePipeline` | `deferred/frame_pipeline.cpp` | `ClusteredLightCull` pass invokes culler when wired |

CUDA `build_cluster_aabbs_kernel` / `cull_lights_kernel` / `deferred_shade_kernel` remain future work.

| Test | Validates |
|------|-----------|
| `fuse_clustered_light_culler` | Cluster count/index encode-decode, screen-depth mapping, slice depth distribution, light-grid rebuild + overflow clamp, culler init, CPU cull assignment + capacity stats, empty scene, contiguous offsets, deferred pipeline wiring |
| `fuse_deferred_pipeline` | Full B5.1 schedule (includes `clustered_light_cull` pass name) |

---

## B5.5 — Shadow System

**Status:** CSM layout + shadow atlas allocation + `ShadowPass` render-graph node landed; cascade split helpers (uniform/log/practical schemes, count clamp) and light-space AABB fitting deepened (B5.5 follow-up).

| Component | Location | Notes |
|-----------|----------|-------|
| `CascadedShadowMapLayout` | `shadow/csm.hpp` | 4 cascades, uniform/log/practical split schemes, cascade count/index clamp, near/far split distances, batch near/far/range helpers, split + range validation, frustum corners, R32F depth |
| `CascadeLightSpaceLayout` | `shadow/csm.hpp` | Light-view matrix, world-frustum-corner → light-space AABB (CPU stub), empty-frustum detection |
| `ShadowAtlas` | `shadow/shadow_atlas.hpp` | Single atlas backing all cascades |
| `DirectionalShadow` | `shadow/directional_shadow.hpp` | Ortho projection fitted from light-space AABB + texel stabilisation |
| `ShadowPass` | `shadow/shadow_pass.hpp` | Records `shadow_maps` into render graph |

SDF soft shadows and deferred shading sampling deferred to B2.6 interop + B5.4 CUDA shade kernel.

| Test | Validates |
|------|-----------|
| `fuse_shadow_system` | Cascade split schemes (uniform/log/practical), count clamp, near/far/range splits, split monotonicity, frustum corners, light-space AABB contain + empty frustum, atlas layout, allocation, shadow pass graph |

---

## B5.6 — Global Illumination: DDGI

**Status:** CPU-first probe grid, irradiance cache, update/sample stubs landed; probe grid indexing + trilinear irradiance lerp deepened (B5 follow-up).

| Component | Location | Notes |
|-----------|----------|-------|
| `DDGIDesc` / `ProbeVolume` | `gi/ddgi.hpp` | Default 16×8×16 = 2048 probes, 64/frame round-robin |
| `ProbeGridCoord` / `ProbeGridLayout` | `gi/ddgi.hpp` | Index↔coord, world→grid, irradiance/depth atlas texel origins |
| `ddgi_util::lerpIrradiance` / `trilinearProbeIrradiance` | `gi/ddgi.cpp` | Spatial probe irradiance interpolation for CPU sample path |
| `DDGI::update` / `sampleIrradiance` | `gi/ddgi.cpp` | CPU hysteresis blend; trilinear cache sample; CUDA kernel launch stub |
| `ddgi_kernels.hpp` | `gi/ddgi_kernels.hpp` | `probe_trace_kernel`, `probe_blend_kernel` stubs |

| Test | Validates |
|------|-----------|
| `fuse_ddgi` | Grid math, probe indexing round-trip, atlas layout, trilinear lerp, scheduling, hysteresis, init/update/sample, pipeline slot |

---

## B5.7 — Screen-Space Effects (CUDA)

**Status:** SSAO / SSR / SSGI API + job wiring landed in `fuse_compute`; B5.7 follow-up expanded stub params, contact-harden helpers, and CPU validation tests. Full kernels and G-buffer interop deferred.

See [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) for component table, backend modes, contact helpers, and deferred pipeline slot (`DeferredPassId::ScreenSpaceAo`).

| Test | Validates |
|------|-----------|
| `fuse_screen_space_effects_stub` | CPU reference samples, blur/contact helpers, param validation, launches, `submit_*_job` counter signal |

---

## B5.8 — Atmosphere & Sky

**Status:** B5.8 deepen — transmittance LUT indexing stubs, sun disk helpers, expanded CPU tests landed.

| Component | Location | Notes |
|-----------|----------|-------|
| `AtmosphereParams` | `atmosphere/atmosphere_params.hpp` | Earth radius, Rayleigh/Mie coefficients |
| `TransmittanceLut` | `atmosphere/transmittance_lut.hpp` | Altitude × cos(zenith) transmittance LUT + flat-index helpers |
| `SkyLut` | `atmosphere/sky_lut.hpp` | Precomputed sky colour LUT |
| `sun_disk` helpers | `atmosphere/sun_disk.hpp` | 0.5° apparent-diameter disk cone + compositing stub |
| `SkyPass` | `atmosphere/sky_pass.hpp` | Depth-tested sky fill via LUT (GPU deferred) |

| Test | Validates |
|------|-----------|
| `fuse_atmosphere_sky` | Phase functions, transmittance indexing/build/sample, sun disk helpers, sky colour stub, LUT build/sample, graph node |

---

## B5.9 — Temporal Anti-Aliasing

**Status:** Halton jitter layout helpers, history validity flags, and resolve bookkeeping deepened (B5.9 follow-up); CUDA kernel deferred.

| Component | Location | Notes |
|-----------|----------|-------|
| `TaaJitterLayout` / `TaaJitter` | `taa/taa_jitter.hpp` | Halton (2,3) reference, configurable sequence length (≤64) |
| `TaaHistoryBuffer` | `taa/taa_history.hpp` | Ping-pong colour history + `hasValidHistory` / `accumulatedFrames` |
| `TaaResolve` / `TaaPass` | `taa/taa_resolve.hpp`, `taa/taa_pass.hpp` | Resolve stub records first-frame + validity; CUDA kernel deferred |

| Test | Validates |
|------|-----------|
| `fuse_taa_pass` | Halton layout, custom sequence length, history validity, ping-pong, resolve stub, graph hook |

---

## B5.10 — Post-Processing Stack

**Status:** CPU-first `PostStack` scaffold landed — bloom, ACES/neutral tonemap, color grade stages; **B5.10 deepen** adds tonemap curve presets, EMA exposure adaptation, and log-luminance histogram metering stubs.

| Component | Location | Notes |
|-----------|----------|-------|
| `PostStack` | `include/fuse/renderer/postprocess/post_stack.hpp` | Three-stage chain: bloom → tonemap → color grade |
| `Bloom` / `ToneMap` / `ColorGrade` | `postprocess/bloom.hpp`, `tonemap.hpp`, `color_grade.hpp` | CPU pixel path for unit tests |
| `TonemapCurve` | `postprocess/tonemap_curve.hpp` | Filmic / Reinhard / ACES curve presets (`make_reinhard_curve_params`, `make_aces_curve_params`) |
| `AutoExposure` / `ExposureMeter` | `postprocess/auto_exposure.hpp` | Average metering + optional EMA adaptation (`use_ema_adaptation`) |
| `LuminanceHistogram` | `postprocess/auto_exposure.hpp` | Log-luminance binning + percentile metering stub |

| Test | Validates |
|------|-----------|
| `fuse_post_process_b510` | Bloom threshold, ACES clamp, neutral 0.18 grey calibration, stage chain, tonemap curve identity/rolloff/Reinhard+ACES clamp, EMA convergence, empty histogram, EV metering/adaptation, auto-exposure integration |

DOF, motion blur, film grain GPU shader chain, and CUDA histogram reduction remain future work.

---

## B5.11 — Lens Flare & Volumetric Lighting

**Status:** CPU-first volumetric fog, light shafts, and lens flare scaffolds wired into deferred pipeline; froxel grid indexing + density lerp helpers deepened (B5.11 follow-up).

| Component | Location | Notes |
|-----------|----------|-------|
| `VolumetricFog` | `include/fuse/renderer/volumetric/volumetric_fog.hpp` | Exponential height fog density + CUDA pass hook |
| `FroxelGridDesc` / `FroxelGridLayout` | `include/fuse/renderer/volumetric/volumetric_fog.hpp` | Froxel index encode/decode, tile/slice clamp helpers, `clampCounts` limits |
| `FroxelSliceLayout` / `froxel_util` | `include/fuse/renderer/volumetric/volumetric_fog.hpp` | Exponential slice bounds, `computeSliceZFromDepth`, bilinear/trilinear density lerp |
| `LightShafts` | `include/fuse/renderer/volumetric/light_shafts.hpp` | Depth-occlusion shaft scaffold |
| `LensFlare` | `include/fuse/renderer/postprocess/lens_flare.hpp` | Ghost/halo generation + pass hook |

| Test | Validates |
|------|-----------|
| `fuse_volumetric_lighting_b511` | Fog density falloff, froxel indexing/clamp/count limits, slice layout, density lerp extremes, empty-scene path, analytic populate, shaft occlusion, lens flare generation, 19-pass graph hooks |

---

## B5.12 — Phase 5 Deliverables & Test Suite

**Status:** Headless integration test exercises `DeferredRenderer` + all B5.1–B5.9 subsystems together; full production gates from P5 §5.12 remain deferred.

| Deliverable | Location | B5.12 status |
|-------------|----------|--------------|
| Phase 5 integration test | `Source/FUSE/Renderer/tests/test_phase5_deferred_integration.cpp` | **Done** — `fuse_phase5_deferred_integration` (headless) |
| Per-component unit tests | `Source/FUSE/Renderer/tests/`, `Source/FUSE/Compute/tests/` | **Done** — B5.1–B5.9 targets listed below |
| CI umbrella run | `.github/workflows/fuse-umbrella-linux.yml` | **Done** — Lavapipe headless ICD; no window surface |

### Integration test flow (headless)

`fuse_phase5_deferred_integration` validates the full B5.1–B5.9 wiring in one executable:

1. `fuse::core::initialize()`
2. `VulkanBootstrap` + `ResourceManager` + `DeferredRenderer` (G-buffer, materials, cluster culler)
3. Initialise satellite subsystems on shared resources: `DirectionalShadow`, `ShadowPass`, `DDGI`, `TaaPass`, `SkyPass`
4. Per frame (3×): shadow update, DDGI probe update, cluster cull, TAA jitter advance, sky pass record, `DeferredRenderer::executeFrame`
5. Asserts: all non-culled passes executed, key pass names present in `CommandBufferRecorder`, 12 Vulkan + 7 CUDA pass split, barriers planned, subsystem stats advance

### Checklist — scaffold landed (B5.1–B5.9) vs deferred (full P5 gates)

#### G-Buffer & Materials

| Item | Status | Notes |
|------|--------|-------|
| G-buffer octahedral normal round-trip < 0.001 angular error | **Done** | `fuse_gbuffer` |
| PBR dielectric→metallic smooth transition (visual) | **Deferred** | No on-screen present in CI |
| Procedural wood/metal/concrete on SDF surfaces | **Deferred** | Material procedural flags scaffold only |
| Material SSBO bindless lookup 1000 materials (RenderDoc) | **Deferred** | SSBO allocation stub; no descriptor pool |
| Emissive surfaces contribute radiance to GI probes | **Deferred** | DDGI sample stub; no emissive feed-through |

#### Lighting

| Item | Status | Notes |
|------|--------|-------|
| Cluster culler assigns zero lights to empty clusters | **Done** | `fuse_clustered_light_culler` empty-scene test |
| Light grid rebuild produces contiguous offsets | **Done (stub)** | `ClusterLightGridLayout::rebuildLightGrid` + `validateContiguousOffsets` |
| Per-cluster light capacity clamp + overflow stats | **Done (stub)** | `maxLightsPerCluster` clamp in cull + rebuild; `clustersAtCapacity` / `lightsDroppedOverflow` stats |
| 1000 point lights — no light leaking (visual) | **Deferred** | CPU cull stub only |
| Clustered cull + deferred shade < 3 ms @ 1080p (CUDA events) | **Deferred** | No CUDA shade kernel |
| CSM correct shadow across 4 cascades (visual) | **Deferred** | Allocation + matrix stubs only |
| CSM stabilisation eliminates shimmer | **Deferred** | — |
| SDF soft shadows correct penumbra | **Deferred** | B2.7 ray march stub |
| SDF shadow vs path tracer within 5% luminance | **Deferred** | — |

#### Global Illumination

| Item | Status | Notes |
|------|--------|-------|
| DDGI probes initialise and first update without CUDA error | **Done** | `fuse_ddgi` |
| 2048 probes, 64/frame update < 2 ms | **Deferred** | CPU stub; no perf gate |
| Irradiance responds to dynamic lights within 64 frames | **Deferred** | Hysteresis math tested; no scene hookup |
| Sun rotation changes GI colour cast (timelapse) | **Deferred** | — |
| GI on white Lambertian within 10% of Monte Carlo | **Deferred** | — |

#### Temporal & Screen-Space

| Item | Status | Notes |
|------|--------|-------|
| TAA eliminates aliasing on edges (visual) | **Deferred** | Jitter + history scaffold only |
| TAA history rejection on fast movers | **Deferred** | Resolve stub; no velocity wiring |
| HBAO correct occlusion in corners | **Deferred** | SSAO CPU reference sample only |
| SSR reflects floor colour within 15% of ground truth | **Deferred** | SSR CPU reference sample only |

#### Atmosphere & Sky

| Item | Status | Notes |
|------|--------|-------|
| Sky Rayleigh scattering — blue midday, orange/red low sun | **Done (stub)** | CPU reference colour; `fuse_atmosphere_sky` |
| No banding in sky gradient at 10-bit | **Deferred** | — |
| Sun disk 0.5° apparent diameter | **Done (stub)** | `sun_disk.hpp` angular radius + compositing; visual gate deferred |
| Volumetric fog exponential falloff | **Done (stub)** | `fuse_volumetric_lighting_b511` CPU density test |

#### Post-Processing

| Item | Status | Notes |
|------|--------|-------|
| Bloom threshold gate | **Done (stub)** | `fuse_post_process_b510` black-frame test |
| Tonemap curve S-curve rolloff | **Done (stub)** | `fuse_post_process_b510` filmic curve tests |
| Reinhard/ACES curve preset clamp | **Done (stub)** | `fuse_post_process_b510` Reinhard+ACES curve clamp tests |
| Auto-exposure EMA adaptation | **Done (stub)** | `fuse_post_process_b510` EMA convergence test |
| Log-luminance histogram metering | **Done (stub)** | `fuse_post_process_b510` empty histogram + percentile metering |
| Auto-exposure EV metering/adaptation | **Done (stub)** | `fuse_post_process_b510` luminance→EV + clamp tests |
| DOF circle of confusion thin-lens formula | **Deferred** | — |
| Motion blur velocity trail | **Deferred** | — |
| ACES tonemap 0.18 grey calibration | **Deferred** | Neutral pass-through calibrated; ACES sRGB gate deferred |
| Film grain temporally decorrelated | **Deferred** | — |

#### Full Frame Performance (RTX 3090, 1920×1080)

| Item | Status | Notes |
|------|--------|-------|
| Total GPU frame time < 16 ms (60 fps) | **Deferred** | No present path / perf gate in CI |
| Per-pass timing gates (depth, G-buffer, DDGI, TAA, etc.) | **Deferred** | — |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_gbuffer` | B5.2 — formats, octahedral encoding, allocation |
| `fuse_material_system` | B5.3 — pack/unpack, SSBO table |
| `fuse_deferred_pipeline` | B5.1 — 16-pass schedule, graph build, execute stub |
| `fuse_clustered_light_culler` | B5.4 — cluster grid, slice depth, light-grid rebuild, light cull, pipeline wiring |
| `fuse_shadow_system` | B5.5 — CSM near/far/range splits, light-space AABB, validation, atlas, shadow pass graph |
| `fuse_ddgi` | B5.6 — probe grid, update/sample, pipeline slot |
| `fuse_screen_space_effects_stub` | B5.7 — SSAO/SSR/SSGI stubs (`fuse_compute`) |
| `fuse_atmosphere_sky` | B5.8 — scatter, LUT, sky pass graph |
| `fuse_taa_pass` | B5.9 — jitter, history, resolve, graph hook |
| `fuse_post_process_b510` | B5.10 — bloom, tonemap, color grade stage chain, tonemap curve, auto-exposure |
| `fuse_volumetric_lighting_b511` | B5.11 — fog, light shafts, lens flare, 19-pass graph hooks |
| `fuse_phase5_deferred_integration` | **B5.12** — full deferred pipeline + all subsystems headless multi-frame |

Run Phase 5 suite:

```bash
ctest --test-dir build --output-on-failure -R 'fuse_gbuffer|fuse_material_system|fuse_deferred_pipeline|fuse_clustered_light_culler|fuse_shadow_system|fuse_ddgi|fuse_atmosphere_sky|fuse_taa_pass|fuse_post_process_b510|fuse_volumetric_lighting_b511|fuse_phase5_deferred'
```

Screen-space effects (separate `fuse_compute` target):

```bash
ctest --test-dir build --output-on-failure -R 'fuse_screen_space_effects'
```

---

## Gates

### B5.1–B5.4 (deferred architecture + G-buffer + materials + clustered shading)

- [x] **B5.1** `DeferredFramePipeline` 16-pass schedule on FUSE APIs
- [x] **B5.2** G-buffer six-attachment layout + octahedral normal encoding
- [x] **B5.3** `MaterialSystem` bindless SSBO scaffold
- [x] **B5.4** `ClusteredLightCuller` CPU stub + light-grid rebuild helpers + deferred pipeline wiring
- [x] CTest targets green under umbrella CI (`FUSE_BUILD_VULKAN=ON`)
- [ ] ASan/UBSan on renderer subsystem tests (umbrella ASan covers runtime smoke; renderer-specific ASan optional follow-up)

### B5.5–B5.9 (shadows, GI, screen-space, atmosphere, TAA)

- [x] **B5.5** `DirectionalShadow` + `ShadowPass` scaffold + cascade split helpers + light-space AABB fitting
- [x] **B5.6** `DDGI` probe grid + update/sample stubs
- [x] **B5.7** SSAO/SSR/SSGI API + job wiring in `fuse_compute`
- [x] **B5.8** `SkyPass` + Rayleigh/Mie CPU reference + LUT
- [x] **B5.9** `TaaPass` jitter + history + resolve facade
- [ ] Full CUDA kernel implementations (deferred — B2.6 interop prerequisite)

### B5.10–B5.11 (post-process, lens flare / volumetric)

- [x] **B5.10** `PostStack` CPU scaffold — bloom, tonemap, color grade (`fuse_post_process_b510`)
- [x] **B5.10 deepen** Tonemap curve presets (Reinhard/ACES), EMA exposure adaptation, log-luminance histogram metering
- [x] **B5.11** Volumetric fog, light shafts, lens flare scaffolds + graph hooks (`fuse_volumetric_lighting_b511`)
- [ ] GPU post-process shader chain (deferred — B2.4 bindless descriptor pool)
- [ ] Full volumetric fog CUDA kernel (deferred — B2.6 interop)

### B5.12 — Phase 5 deliverables & integration

- [x] **B5.12** End-to-end test: `DeferredRenderer` + shadows + DDGI + TAA + sky + materials + cluster cull (`fuse_phase5_deferred_integration`)
- [x] All non-culled deferred passes execute and record in headless multi-frame test
- [x] Phase 5 CTest targets registered and green under umbrella CI
- [ ] Master-plan visual/perf baselines (RenderDoc, RTX 3090 timing) — deferred to GPU-present path

---

## Next

- [ ] Wire `DeferredRenderer` into `RhiContext::submitFrame` (replace hybrid placeholder incrementally)
- [x] B5.4 follow-up: Cluster grid index/decode/screen-depth helpers, slice-Z mapping, overflow clamp + CPU tests (`fuse_clustered_light_culler`)
- [ ] B5.4 follow-up: CUDA cluster AABB build + deferred shade kernels
- [ ] B5.5 follow-up: SDF soft shadows in `fuse_compute` (CSM split + light-space AABB stubs landed)
- [ ] B5.7 follow-up: G-buffer `cudaInterop` surface import via B2.6 (stub params + contact-harden CPU helpers landed)
- [ ] B5.9 follow-up: CUDA `taa_resolve_kernel` (CPU jitter/history validity stub landed)
- [ ] B5.10 follow-up: DOF / motion blur / film grain GPU shader chain; CUDA histogram auto-exposure
- [x] B5.11 follow-up: Froxel grid indexing stubs, density lerp helpers, CPU tests (`fuse_volumetric_lighting_b511`)
- [ ] B5.11 follow-up: Full volumetric fog CUDA kernel + lens flare GPU composite
- [ ] Scene `SceneData` → deferred G-buffer draw list handoff (B3 → B5 bridge)
- [ ] Editor Qt viewport deferred preview (B6)

---

## Related docs

- [TRACK-B-RENDER.md](./TRACK-B-RENDER.md) — B5.2/B5.3/B5.5/B5.6/B5.9 deepen summary (G-buffer validation, material blocks, CSM AABB, DDGI probe grid, TAA jitter/history)
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — RHI bootstrap, render graph, CUDA job lane (B2)
- [TRACK-B-ECS.md](./TRACK-B-ECS.md) — scene data producer (B3)
- [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) — SSAO/SSR/SSGI detail
- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B5
- [Source/FUSE/Renderer/README.md](../../Source/FUSE/Renderer/README.md) — module overview
