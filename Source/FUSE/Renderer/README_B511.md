# B5.11 — Lens Flare & Volumetric Lighting

CPU side of Track B5.11 under `fuse_rhi`: exponential height fog with a closed-form optical depth and a segment-exact march (the fog falloff gate is proven by `fuse_b5_atmosphere_gates`), analytic lens-flare generation, and a light-shaft occlusion scaffold, with render-graph pass hooks wired into `DeferredFramePipeline`. The CUDA fog kernel and GPU flare/shaft composites are not written yet.

## Layout

| Header | Role |
|--------|------|
| `postprocess/lens_flare.hpp` | `LensFlareSample`, `generate_lens_flare`, lens-flare pass hook |
| `volumetric/volumetric_fog.hpp` | `VolumetricFogParams`, froxel grid indexing, density lerp, analytic + marched optical depth / transmittance, CUDA pass hook |
| `volumetric/light_shafts.hpp` | `LightShaftsParams`, screen-space occlusion stub, pass hook |

## Frame pipeline hooks

`DeferredFramePipeline::buildGraph` inserts:

1. **Volumetric fog** — CUDA pass after screen-space AO (depth read)
2. **Light shafts** — graphics pass after atmosphere/sky
3. **Lens flare** — graphics pass after post-process stack

## Tests

`fuse_volumetric_lighting` (`ctest` name `fuse_volumetric_lighting_b511`) covers fog density falloff, froxel grid indexing/clamp/count limits (including oversized decode and depth/screen rejection), exponential slice layout, density lerp extremes, screen-space trilinear sampling, empty-scene path (zero density / zero march / zero-dimension grid), bilinear/trilinear density lerp, analytic fog populate, light-shaft occlusion, lens-flare element generation, pass recording, and deferred pipeline pass registration.

## Upstream / downstream

- **Depends on:** B5.1 frame pipeline, B5.10 post-process stack ordering
- **Future:** CUDA kernels (`volumetric_fog_kernel`, `lens_flare_composite_kernel`) and shadow sampling (B5.5)
