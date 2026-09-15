# fuse_rhi atmosphere — B5.8 Atmosphere & Sky (stub)

CPU-first Rayleigh/Mie sky scaffolding for P5 §5.8. Provides `AtmosphereParams`, analytic phase helpers, a sun/sky LUT table, and a `SkyPass` render-graph hook. CUDA `sky_kernel` and GPU LUT upload are deferred.

## Layout

| Header | Role |
|--------|------|
| `atmosphere_params.hpp` | Physical atmosphere constants |
| `sky_scatter.hpp` | CPU `rayleigh_phase`, `mie_phase`, `compute_sky_colour` stub |
| `sky_lut.hpp` | Sun elevation × view elevation LUT builder |
| `sky_pass.hpp` | Pass scaffold + render-graph insertion |

## Pipeline (stub)

`transparent → atmosphere_sky (LUT sample, depth test) → TAA`

The deferred frame pipeline already reserves `DeferredPassId::AtmosphereSky`; this module supplies the host-side types and a standalone graph helper.

## Tests

`fuse_atmosphere_sky` (`ctest` name `fuse_atmosphere_sky`) covers parameter defaults, phase functions, sky colour stub behaviour, LUT build/sample, pass recording, and graph compilation.

## Build

Built with `FUSE_BUILD_VULKAN=ON`. Tests run when `FUSE_BUILD_CORE_TESTS=ON`.

## Upstream / downstream

- **Depends on:** B5.1 deferred frame pipeline slot
- **Future:** CUDA `sky_kernel`, volumetric fog (P5 §5.11), GPU LUT upload
