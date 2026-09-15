# B5.11 — Lens Flare & Volumetric Lighting (stub)

CPU-first scaffolding for Track B5.11 under `fuse_rhi`. Implements analytic lens flare generation, volumetric fog density sampling, and light-shaft occlusion stubs with render-graph pass hooks wired into `DeferredFramePipeline`.

## Layout

| Header | Role |
|--------|------|
| `postprocess/lens_flare.hpp` | `LensFlareSample`, `generate_lens_flare`, lens-flare pass hook |
| `volumetric/volumetric_fog.hpp` | `VolumetricFogParams`, froxel grid indexing, density lerp helpers, CUDA pass hook |
| `volumetric/light_shafts.hpp` | `LightShaftsParams`, screen-space occlusion stub, pass hook |

## Frame pipeline hooks

`DeferredFramePipeline::buildGraph` inserts:

1. **Volumetric fog** — CUDA pass after screen-space AO (depth read)
2. **Light shafts** — graphics pass after atmosphere/sky
3. **Lens flare** — graphics pass after post-process stack

## Tests

`fuse_volumetric_lighting` (`ctest` name `fuse_volumetric_lighting_b511`) covers fog density falloff, froxel grid indexing, exponential slice layout, bilinear/trilinear density lerp, analytic fog populate, light-shaft occlusion, lens-flare element generation, pass recording, and deferred pipeline pass registration.

## Upstream / downstream

- **Depends on:** B5.1 frame pipeline, B5.10 post-process stack ordering
- **Future:** CUDA kernels (`volumetric_fog_kernel`, `lens_flare_composite_kernel`) and shadow sampling (B5.5)
