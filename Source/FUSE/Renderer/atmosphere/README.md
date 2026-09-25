# fuse_rhi atmosphere — B5.8 Atmosphere & Sky

CPU reference of the P5 §5.8 sky: a real Rayleigh + Mie **single-scattering integral** over a
spherical planet (view-ray in-scatter with sun transmittance, planet shadow with a disk penumbra),
a limb-darkened 0.5° sun disk, precomputed transmittance/sky LUTs, and TPDF-dithered 10-bit output.
The `SkyPass` render-graph hook samples the LUT; the CUDA `sky_kernel` and GPU LUT upload are not
written yet.

## Layout

| Header | Role |
|--------|------|
| `atmosphere_params.hpp` | Physical atmosphere constants |
| `sky_scatter.hpp` | Phase functions, optical depth / transmittance along spherical-shell rays, single-scattering `compute_sky_inscatter` / `compute_sky_colour` |
| `transmittance_lut.hpp` | Altitude × cos(zenith) transmittance LUT + flat-index helpers |
| `sky_lut.hpp` | Sun elevation × view elevation sky colour LUT builder |
| `sun_disk.hpp` | 0.5° sun disk: angular separation, limb-darkened radiance, solid-angle integral |
| `sky_output.hpp` | TPDF dither + unorm quantisation for 10-bit output |
| `sky_pass.hpp` | Pass scaffold + render-graph insertion |

## Behaviour

- **Scattering** — `compute_sky_inscatter` integrates in-scatter along the view ray (samples dense near
  the camera) with per-sample sun transmittance; `compute_sky_colour` adds the transmitted sun disk.
  Within 5.9% radiance / 0.0033 chroma of a brute-force reference; blue at noon, red at sunset.
  Phase functions integrate to 1.
- **LUTs** — `TransmittanceLut` (altitude × cos zenith) and `SkyLut` (sun × view elevation; the
  elevation axis was inverted and is fixed). Both sample the **nearest bin**; bilinear sampling is
  future work.
- **Sun disk** — hard edge at 0.25° angular radius (0.5° diameter), linear limb darkening, solid-angle
  normalised to 1.
- **Output** — `sky_dither_tpdf` + `sky_quantize_unorm`: 10-bit output with no visible banding.

## Tests

- `fuse_b5_atmosphere_gates` — B5.8/B5.11 gate rows: Rayleigh/Mie scattering vs brute force, 10-bit
  banding, sun disk diameter, volumetric fog falloff vs closed form.
- `fuse_atmosphere_sky` — parameter defaults, phase functions, LUT indexing/build/sample, pass
  recording and graph compilation.

## Build

Part of `fuse_rhi` (CPU code builds with or without Vulkan). Tests run when `FUSE_BUILD_CORE_TESTS=ON`.

## Upstream / downstream

- **Depends on:** B5.1 deferred frame pipeline slot
- **Future:** CUDA `sky_kernel`, GPU LUT upload, bilinear LUT sampling
