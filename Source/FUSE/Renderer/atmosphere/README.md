# fuse_rhi atmosphere — B5.8 Atmosphere & Sky (deepen)

CPU-first Rayleigh/Mie sky scaffolding for P5 §5.8. Provides `AtmosphereParams`, analytic phase helpers, transmittance and sun/sky LUT tables, sun-disk compositing, and a `SkyPass` render-graph hook. CUDA `sky_kernel` and GPU LUT upload are deferred.

## Layout

| Header | Role |
|--------|------|
| `atmosphere_params.hpp` | Physical atmosphere constants |
| `sky_scatter.hpp` | CPU `rayleigh_phase`, `mie_phase`, `compute_sky_colour` stub |
| `transmittance_lut.hpp` | Altitude × cos(zenith) transmittance LUT + flat-index helpers |
| `sky_lut.hpp` | Sun elevation × view elevation sky colour LUT builder |
| `sun_disk.hpp` | 0.5° apparent-diameter sun disk helpers |
| `sky_pass.hpp` | Pass scaffold + render-graph insertion |

## Transmittance LUT indexing (stub)

`transmittance_lut_flat_index`, `transmittance_lut_decode_index`, and `transmittance_lut_index_from_samples` map physical altitude and cos(zenith) to row-major bins. Bilinear sampling and GPU upload are deferred — nearest-bin lookup only.

## Sun disk (stub)

`sun_angular_radius_rad()` returns half of the P5 0.5° apparent diameter. `composite_sun_disk` adds a smooth radiance lobe when the view ray lies inside the disk cone. Full limb darkening and HDR exposure are deferred.

## Pipeline (stub)

`transparent → atmosphere_sky (LUT sample, depth test) → TAA`

The deferred frame pipeline already reserves `DeferredPassId::AtmosphereSky`; this module supplies the host-side types and a standalone graph helper.

## Tests

`fuse_atmosphere_sky` (`ctest` name `fuse_atmosphere_sky`) covers parameter defaults, phase functions, transmittance indexing/build/sample, sun disk helpers, sky colour stub behaviour, LUT build/sample, pass recording, and graph compilation.

## Build

Built with `FUSE_BUILD_VULKAN=ON`. Tests run when `FUSE_BUILD_CORE_TESTS=ON`.

## Upstream / downstream

- **Depends on:** B5.1 deferred frame pipeline slot
- **Future:** CUDA `sky_kernel`, volumetric fog (P5 §5.11), GPU LUT upload, bilinear transmittance sampling
