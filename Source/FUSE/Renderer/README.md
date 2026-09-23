# fuse_rhi — Renderer module

Vulkan RHI and deferred renderer from [FUSE Master Plan](../../docs/plans/FUSE_MASTER_PLAN.md).

## B5.6 — DDGI

Dynamic Diffuse Global Illumination (P5 §5.6) with a real CPU path: probes trace the scene,
blend into the octahedral irradiance/depth atlases with hysteresis plus change detection, and
`sampleIrradiance` does the trilinear, visibility-weighted lookup. Proven by `fuse_b5_ddgi_gates`
(0.96% vs Monte Carlo, 64-frame light response, sun colour cast, emissive 0.85% vs form factor).
The CUDA probe trace/blend kernels are not written yet.

| Header | Role |
|--------|------|
| `gi/ddgi.hpp` | `DDGIDesc`, `ProbeVolume`, `ProbeValidityFlags`, `DdgiIrradianceEncoding`, `DDGI` lifecycle + `sampleIrradiance` |
| `gi/ddgi_kernels.hpp` | CUDA kernel launch stubs (`probe_trace_kernel`, `probe_blend_kernel`); the CPU path is the reference |

### Defaults

- 16×8×16 probe grid (2048 probes)
- 64 probes updated per frame (round-robin)
- Octahedral irradiance atlas + depth variance atlas via `ResourceManager`

## B5.10 — Post-Processing Stack

CPU post-processing (P5 §5.10), proven by `fuse_b5_post_gates`: energy-conserving bloom pyramid with a
continuous knee, thin-lens depth of field, velocity motion blur, ACES calibrated so scene 0.18 maps to
display 0.18, per-pixel temporally decorrelated film grain, and auto-exposure. Stage order:

`bloom → depth_of_field → motion_blur → tonemap → color_grade → film_grain` (`PostStack::processFrame`)

The CUDA/Vulkan post passes and GPU histogram reduction are not written yet.

| Header | Role |
|--------|------|
| `postprocess/bloom.hpp` | `BloomParams`, soft-knee threshold + downsample/upsample pyramid |
| `postprocess/tonemap_curve.hpp` | `TonemapCurveParams`, filmic/Reinhard/ACES curve presets |
| `postprocess/tonemap.hpp` | `ToneMapper`, `aces_tonemap()`, host tone-map pass |
| `postprocess/auto_exposure.hpp` | `AutoExposureParams`, `ExposureMeter`, `LuminanceHistogram`, EMA adaptation |
| `postprocess/color_grade.hpp` | `ColorGradeParams`, exposure/saturation/CDL |
| `postprocess/dof.hpp` | Thin-lens circle of confusion + gather blur |
| `postprocess/motion_blur.hpp` | Velocity-buffer motion blur with shutter angle and length clamp |
| `postprocess/post_stack.hpp` | `PostStack` facade chaining the stages |

## Tests (`ctest`)

| Test | Coverage |
|------|----------|
| `fuse_b5_ddgi_gates`, `fuse_b5_ddgi_timing` | B5.6 gate rows (CPU reference); timing reports CPU cost only |
| `fuse_b5_post_gates` | B5.10 gate rows: bloom, DoF, motion blur, ACES calibration, film grain |
| `fuse_ddgi` | Grid math, probe indexing, border/interior validity, octahedral direction encoding, atlas texel layout, lerp extremes, OOB clamp, trilinear irradiance lerp, hysteresis blend, probe scheduling, init/update/sample, pipeline slot |
| `fuse_post_process_b510` | Bloom thresholding, ACES clamping, neutral 0.18 grey calibration, tonemap curve rolloff/Reinhard+ACES clamp, EMA convergence, empty histogram, auto-exposure EV metering/adaptation, stage ordering, `PostStack` facade |
