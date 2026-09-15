# Track B — Own Renderer (B5 deferred pipeline)

**Status:** B5.2 G-buffer layout validation + B5.3 PBR material parameter blocks + **B5.4 clustered light grid deepen** + **B5.5 CSM split-distance validation + batch light-space matrices deepen** + **B5.10 post-process tonemap/auto-exposure deepen** + **B5.11 volumetric froxel grid deepen** + B5.6 DDGI probe grid indexing deepened + B5.9 TAA jitter/history deepened  
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
| `CascadedShadowMapLayout` | Uniform/log/practical split schemes, cascade count clamp, batch near/far/range + split-distance helpers, `validateSplitDistances`, `isEmptyCascadeFrustum`, cascade frustum corners, split + range validation |
| `CascadeLightSpaceLayout` | Light-view matrix, world-frustum → light-space AABB, ortho bounds fit + `validateOrthoBounds`, texel stabilisation, `buildAllCascadeLightSpaceMatrices` batch builder, degenerate light-direction guard |
| `DirectionalShadow` | Delegates cascade view-projection to `buildCascadeLightSpaceMatrices` |
| `fuse_shadow_system` | Split schemes, variable/single cascade count, split-distance validation, ortho bounds validation, batch matrix builder, empty/degenerate frustum paths, atlas, shadow pass graph |

```bash
ctest --test-dir build --output-on-failure -R fuse_shadow_system
```

---

## B5.4 deepen — Clustered light grid / tile assignment (CPU)

| Component | Notes |
|-----------|-------|
| `ClusterDesc::clampCounts` / `isEmpty` | Tile/slice/light caps clamped to CPU stub maxima; zero dimensions remain empty |
| `ClusterGridSoA::allocate` / `clear` | Resize AABB/grid storage for a clamped desc; clear flat light list |
| `ClusterGridLayout` | Tile/cluster index encode/decode (oversized decode clamp), bounds clamp, screen-depth → cluster index |
| `ClusterSliceLayout` | Exponential slice near/far + `computeSliceZFromDepth` (mirrors froxel layout) |
| `ClusterLightGridLayout` | Flat light-list packing with per-cluster capacity clamp; zero-cluster rebuild clears SoA |
| `cluster_util` | `tryAssignLight`, `assignLights`, `lookupClusterLights`, `countAssignedLights`, `countEmptyClusters` |
| `ClusteredLightCullerStats` | `clustersAtCapacity` / `lightsDroppedOverflow` overflow reporting |
| `fuse_clustered_light_culler` | Desc clamp, index round-trip + oversized decode, zero-dim grid, screen/depth clamp paths, allocate/clear, assignment lookup/counts, overflow |

```bash
ctest --test-dir build --output-on-failure -R fuse_clustered_light_culler
```

---

## B5.11 deepen — Volumetric fog froxel grid (CPU)

| Component | Notes |
|-----------|-------|
| `FroxelGridDesc` / `FroxelDensityGrid` | View-aligned froxel injection grid dimensions + per-froxel density cache; `clampCounts` enforces CPU stub maxima; `allocate`/`clear`/`matchesDesc` manage zero-filled density |
| `FroxelSliceLayout` | Exponential depth-slice near/far bounds + `computeSliceZFromDepth` (mirrors clustered layout) |
| `FroxelGridLayout` | Froxel index encode/decode, `froxelIndexClamped`, tile/slice/index clamp helpers, screen-depth → sample-coords / froxel-index mapping |
| `froxel_util` | `lerpDensity` (clamped t), `countNonZeroFroxels` / `countEmptyFroxels`, bilinear/trilinear + screen-space density sample, analytic fog populate |
| `record_volumetric_fog_pass` | Skips when `march_steps == 0` or `density <= 0` (empty-scene path) |
| `fuse_volumetric_lighting_b511` | Froxel indexing/clamp (OOB coords, zero-dimension grid), density lerp + counts, empty grid / zero march, analytic populate |

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
| `ProbeGridCoord` / `ProbeGridLayout` | Index ↔ coord decode, `clampProbeIndex`, world→grid mapping, `clampWorldToProbeGridCoord`, atlas texel origins |
| `ProbeValidityFlags` | Border/interior/face-edge classification + `has_trilinear_neighbourhood` |
| `DdgiIrradianceEncoding` | Octahedral direction encode/decode, `directionToTexelOffset`, atlas texel lookup |
| `ddgi_util::lerpIrradiance` | Spatial irradiance blend — clamps `t` to [0, 1] |
| `ddgi_util::bilinearTileIrradiance` | Bilinear lerp within a probe's octahedral tile (CPU stub) |
| `ddgi_util::trilinearProbeIrradiance` | 8-corner trilinear sample from CPU cache — OOB world positions clamp to grid edge |
| `DDGI::sampleIrradiance` | Uses trilinear probe interpolation (was nearest-probe) |

`fuse_ddgi` also covers empty-grid scheduling/sample paths, OOB index clamp, and 1×1×1 border validity.

```bash
ctest --test-dir build --output-on-failure -R fuse_ddgi
```

---

## B5.10 deepen — Post-process tonemap curve + auto-exposure (CPU)

| Component | Notes |
|-----------|-------|
| `TonemapCurveKind` / `make_filmic_curve_params` / `make_reinhard_curve_params` / `make_aces_curve_params` | Filmic + Reinhard extended + Hill ACES curve presets applied before tone-map operator |
| `TonemapCurveEndpoints` / `evaluate_tonemap_curve_endpoints` / `tonemap_curve_output_span` | Black/white anchor evaluation, display-range span, `tonemap_curve_preserves_black` calibration guard |
| `apply_exposure_ev` | Shared EV-stop scaling used by `PostStack` before tonemap curve |
| `AutoExposureParams::use_ema_adaptation` | Optional EMA luminance smoothing with asymmetric up/down alpha |
| `ev_to_luminance` / `reset_auto_exposure_state` / `AutoExposure::reset` | Inverse metering + scene-boundary state reset |
| `is_brightening_luminance` / `ema_blend` | Directional EMA helpers used by `update_smoothed_luminance` |
| `LuminanceHistogram` / `histogram_util` | Log-luminance binning (`logBinIndex`, `binCenterLuminance`), batch accumulate, percentile metering, `isEmpty` guard |
| `PostStack::updateAutoExposureFromHistogram` | Histogram metering path into auto-exposure facade |
| `fuse_post_process_b510` | Curve endpoint anchors, EV round-trip, asymmetric EMA adapt, histogram percentile + empty paths, reset + histogram integration |

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
| `TaaJitterLayout::offsetForFrameIndex` / `ndcOffsetForFrameIndex` | Halton/NDC jitter for monotonic frame counters (wraps with period) |
| `TaaJitter::syncToFrameIndex` | Align jitter state to a wrapped monotonic frame counter |
| `TaaJitterLayout::fillHaltonSequence` | Full Halton (2,3) table for projection jitter |
| `TaaHistoryBuffer::hasValidHistory` / `needsWarmup` | Invalid until first resolve; cleared on resize |
| `TaaHistoryBuffer::accumulatedFrames` | Monotonic resolve counter |
| `TaaResolveStats::first_frame` / `effective_blend` | First warm-up frame uses full current weight (1.0) |
| `TaaResolveStats::skipped` | Set when resolve bails before history update |
| `TaaResolve::resetBookkeeping` | Clears resolve stats; `TaaPass::destroy` / `invalidateHistory` reset validity path |
| `fuse_taa_pass` | Layout helpers, sequence period/large-frame wrap, syncToFrameIndex, validity reset, empty history, resolve bookkeeping |

```bash
ctest --test-dir build --output-on-failure -R fuse_taa_pass
```

---

## Related docs

- [TRACK-B-RENDER-B5.md](./TRACK-B-RENDER-B5.md) — full deferred rendering track (B5.1–B5.12)
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — RHI / render graph (B2)
- [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) — SSAO / SSR / SSGI stubs
- [Source/FUSE/Renderer/README.md](../../Source/FUSE/Renderer/README.md)
