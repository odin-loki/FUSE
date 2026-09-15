# Track B — Own Renderer (B5 deferred pipeline)

**Status:** B5.2 G-buffer layout validation + B5.3 PBR material parameter blocks + **B5.4 clustered light grid deepen** + B5.5 CSM light-space AABB deepen + **B5.10 post-process tonemap/auto-exposure deepen** + **B5.11 volumetric froxel grid deepen** + B5.6 DDGI probe grid indexing deepened + B5.9 TAA jitter/history deepened  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B5  
**Detail:** [TRACK-B-RENDER-B5.md](./TRACK-B-RENDER-B5.md) — full B5.1–B5.12 scope, tests, gates  
**Depends on:** [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) (RHI bootstrap, render graph, CUDA job lane)

---

## Scope

| Topic | Location | Notes |
|-------|----------|-------|
| G-buffer layout | `include/fuse/renderer/deferred/gbuffer.hpp` | Six attachments, octahedral normals, CPU MRT pack/unpack |
| PBR materials | `include/fuse/renderer/material/` | Authoring `Material`, `MaterialParameterBlock`, GPU SSBO rows |
| Deferred frame graph | `include/fuse/renderer/deferred/` | 19-pass schedule — see B5 detail doc |
| Shader reference | `Source/FUSE/Renderer/shaders/common/gbuffer.glsl` | `write_gbuffer` kept in sync with `GBufferPacking` |

**Not in scope (deferred):** Real `vkCmd*` G-buffer draws, bindless descriptor pool, CUDA deferred-shade kernels, RenderDoc visual gates.

---

## B5.2 deepen — G-buffer layout validation (CPU)

| Component | Validates |
|-----------|-----------|
| `GBufferLayout::channelCount` | Per-attachment channel counts (RG velocity, R depth, RGBA colour) |
| `GBufferLayout::validateAttachmentFormats` | All six slots map to concrete `GpuFormat` values |
| `GBufferPacking::pack` / `unpack` | CPU MRT channels mirror `gbuffer.glsl` `write_gbuffer` |
| `fuse_gbuffer` | Formats, channel counts, octahedral round-trip, MRT pack round-trip, allocation |

```bash
ctest --test-dir build --output-on-failure -R fuse_gbuffer
```

---

## B5.5 deepen — Shadow cascade / CSM (CPU)

| Component | Notes |
|-----------|-------|
| `CascadedShadowMapLayout` | Uniform/log/practical split schemes, cascade count clamp, batch near/far/range + split-distance helpers, cascade frustum corners, split + range validation |
| `CascadeLightSpaceLayout` | Light-view matrix, world-frustum → light-space AABB, ortho bounds fit + texel stabilisation, per-cascade `CascadeLightSpaceMatrices` bookkeeping |
| `DirectionalShadow` | Delegates cascade view-projection to `buildCascadeLightSpaceMatrices` |
| `fuse_shadow_system` | Split schemes, variable cascade count, split-distance batch, ortho fit/stabilise, light-space matrix bookkeeping, empty/degenerate frustum paths, atlas, shadow pass graph |

```bash
ctest --test-dir build --output-on-failure -R fuse_shadow_system
```

---

## B5.4 deepen — Clustered light grid / tile assignment (CPU)

| Component | Notes |
|-----------|-------|
| `ClusterGridLayout` | Tile/cluster index encode/decode, bounds clamp, screen-depth → cluster index |
| `ClusterSliceLayout` | Exponential slice near/far + `computeSliceZFromDepth` (mirrors froxel layout) |
| `ClusterLightGridLayout` | Flat light-list packing with per-cluster capacity clamp |
| `ClusteredLightCullerStats` | `clustersAtCapacity` / `lightsDroppedOverflow` overflow reporting |
| `fuse_clustered_light_culler` | Index round-trip, screen mapping, rebuild overflow clamp, empty scene, capacity stats |

```bash
ctest --test-dir build --output-on-failure -R fuse_clustered_light_culler
```

---

## B5.11 deepen — Volumetric fog froxel grid (CPU)

| Component | Notes |
|-----------|-------|
| `FroxelGridDesc` / `FroxelDensityGrid` | View-aligned froxel injection grid dimensions + per-froxel density cache; `clampCounts` enforces CPU stub maxima |
| `FroxelSliceLayout` | Exponential depth-slice near/far bounds + `computeSliceZFromDepth` (mirrors clustered layout) |
| `FroxelGridLayout` | Froxel index encode/decode, tile/slice/index clamp helpers, screen-depth → sample-coords mapping |
| `froxel_util` | `lerpDensity` (clamped t), bilinear/trilinear density sample, analytic fog populate |
| `record_volumetric_fog_pass` | Skips when `march_steps == 0` or `density <= 0` (empty-scene path) |
| `fuse_volumetric_lighting_b511` | Froxel indexing/clamp, slice distribution, density lerp extremes, empty-scene path, analytic populate |

```bash
ctest --test-dir build --output-on-failure -R fuse_volumetric_lighting_b511
```

---

## B5.3 deepen — Material parameter blocks

| Component | Notes |
|-----------|-------|
| `MaterialParameterBlock` | `SubsurfaceParams`, `ClearCoatParams`, `ClothParams` authoring stubs |
| `MaterialFlagBits` | Procedural + bindless map presence flags (normal/AO/metallic) |
| `Material::GPUMaterial` | Extension rows: `subsurfaceBlock`, `clearCoatBlock`, `clothBlock`; `metallicTexIdx`, `normalStrength` |
| `MaterialLayout::validateGpuStruct` | SSBO stride / offset checks for CPU tests |
| `fuse_material_system` | Pack extension blocks, texture flags, layout validation, register/flush |

```bash
ctest --test-dir build --output-on-failure -R fuse_material_system
```

---

## B5.6 deepen — DDGI probe grid indexing + irradiance lerp

| Component | Notes |
|-----------|-------|
| `ProbeGridCoord` / `ProbeGridLayout` | Index ↔ coord decode, world→grid mapping, `clampWorldToProbeGridCoord`, atlas texel origins |
| `ProbeValidityFlags` | Border/interior classification + `has_trilinear_neighbourhood` |
| `DdgiIrradianceEncoding` | Octahedral direction encode/decode, `directionToTexelOffset`, atlas texel lookup |
| `ddgi_util::lerpIrradiance` | Spatial irradiance blend — clamps `t` to [0, 1] |
| `ddgi_util::bilinearTileIrradiance` | Bilinear lerp within a probe's octahedral tile (CPU stub) |
| `ddgi_util::trilinearProbeIrradiance` | 8-corner trilinear sample from CPU cache — OOB world positions clamp to grid edge |
| `DDGI::sampleIrradiance` | Uses trilinear probe interpolation (was nearest-probe) |

```bash
ctest --test-dir build --output-on-failure -R fuse_ddgi
```

---

## B5.10 deepen — Post-process tonemap curve + auto-exposure (CPU)

| Component | Notes |
|-----------|-------|
| `TonemapCurveKind` / `make_reinhard_curve_params` / `make_aces_curve_params` | Reinhard extended + Hill ACES curve presets applied before tone-map operator |
| `AutoExposureParams::use_ema_adaptation` | Optional EMA luminance smoothing with asymmetric up/down alpha |
| `LuminanceHistogram` | Log-luminance binning, percentile metering, empty-histogram guard |
| `fuse_post_process_b510` | Reinhard/ACES curve clamp, EMA convergence, empty histogram |

```bash
ctest --test-dir build --output-on-failure -R fuse_post_process_b510
```

---

## B5.9 deepen — TAA jitter sequence + history validity (CPU)

| Component | Validates |
|-----------|-----------|
| `TaaJitterLayout::halton` | CPU Halton reference for bases 2 and 3 |
| `TaaJitterLayout::validateSequenceLength` | Rejects zero or >64 frame sequences |
| `TaaJitterLayout::sequencePeriod` / `frameIndexInSequence` | Jitter cycle length + frame→slot mapping |
| `TaaJitterLayout::fillHaltonSequence` | Full Halton (2,3) table for projection jitter |
| `TaaHistoryBuffer::hasValidHistory` | Invalid until first resolve; cleared on resize |
| `TaaHistoryBuffer::accumulatedFrames` | Monotonic resolve counter |
| `TaaResolveStats::first_frame` | First warm-up frame before history reuse |
| `TaaResolve::resetBookkeeping` | Clears resolve stats; `TaaPass::destroy` resets validity path |
| `fuse_taa_pass` | Layout helpers, sequence period, validity reset, empty history, resolve bookkeeping |

```bash
ctest --test-dir build --output-on-failure -R fuse_taa_pass
```

---

## Related docs

- [TRACK-B-RENDER-B5.md](./TRACK-B-RENDER-B5.md) — full deferred rendering track (B5.1–B5.12)
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — RHI / render graph (B2)
- [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) — SSAO / SSR / SSGI stubs
- [Source/FUSE/Renderer/README.md](../../Source/FUSE/Renderer/README.md)
