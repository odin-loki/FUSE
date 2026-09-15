# Track B — Own Renderer (B5 deferred pipeline)

**Status:** B5.2 G-buffer layout validation + B5.3 PBR material parameter blocks + B5.5 CSM light-space AABB deepen + **B5.11 volumetric froxel grid deepen**  
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
| `CascadedShadowMapLayout` | Batch near/far/range helpers, cascade frustum corners, split + range validation |
| `CascadeLightSpaceLayout` | Light-view matrix + light-space AABB from cascade frustum corners |
| `DirectionalShadow` | Orthographic projection fitted from light-space AABB with texel stabilisation |
| `fuse_shadow_system` | Near/far/range batch, frustum corners, light-space AABB, atlas, shadow pass graph |

```bash
ctest --test-dir build --output-on-failure -R fuse_shadow_system
```

---

## B5.11 deepen — Volumetric fog froxel grid (CPU)

| Component | Notes |
|-----------|-------|
| `FroxelGridDesc` / `FroxelDensityGrid` | View-aligned froxel injection grid dimensions + per-froxel density cache |
| `FroxelSliceLayout` | Exponential depth-slice near/far bounds (mirrors clustered layout) |
| `FroxelGridLayout` | Froxel index encode/decode + screen-depth → sample-coords mapping |
| `froxel_util` | `lerpDensity`, bilinear/trilinear density sample, analytic fog populate |
| `fuse_volumetric_lighting_b511` | Froxel indexing, slice distribution, density lerp, analytic populate |

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

## Related docs

- [TRACK-B-RENDER-B5.md](./TRACK-B-RENDER-B5.md) — full deferred rendering track (B5.1–B5.12)
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — RHI / render graph (B2)
- [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) — SSAO / SSR / SSGI stubs
- [Source/FUSE/Renderer/README.md](../../Source/FUSE/Renderer/README.md)
