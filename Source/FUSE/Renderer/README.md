# fuse_rhi — Renderer module

Vulkan RHI and deferred rendering scaffolds from [FUSE Master Plan](../../docs/plans/FUSE_MASTER_PLAN.md).

## B5.6 — DDGI (stub)

CPU-first Dynamic Diffuse Global Illumination scaffolding (P5 §5.6). Probe grid types, irradiance cache allocation, and update/sample stub APIs are in place; CUDA probe trace kernels are deferred.

| Header | Role |
|--------|------|
| `gi/ddgi.hpp` | `DDGIDesc`, `ProbeVolume`, `IrradianceCacheEntry`, `DDGI` lifecycle + `sampleIrradiance` |
| `gi/ddgi_kernels.hpp` | CUDA kernel launch stubs (`probe_trace_kernel`, `probe_blend_kernel`) |

### Defaults

- 16×8×16 probe grid (2048 probes)
- 64 probes updated per frame (round-robin)
- Octahedral irradiance atlas + depth variance atlas via `ResourceManager`

## B5.10 — Post-Processing Stack (stub)

CPU-first post-processing scaffolding (P5 §5.10). Implements the pipeline as host stubs:

`bloom → tonemap_curve → tonemap → color_grade`

CUDA compute passes, dual-kawase bloom pyramids, and histogram auto-exposure are deferred; the public API mirrors the P5 source narrative.

| Header | Role |
|--------|------|
| `postprocess/bloom.hpp` | `BloomParams`, CPU threshold/extract stub |
| `postprocess/tonemap_curve.hpp` | `TonemapCurveParams`, filmic/Reinhard/ACES curve presets |
| `postprocess/tonemap.hpp` | `ToneMapper`, `aces_tonemap()`, host tone-map pass |
| `postprocess/auto_exposure.hpp` | `AutoExposureParams`, `ExposureMeter`, `LuminanceHistogram`, EMA adaptation stub |
| `postprocess/color_grade.hpp` | `ColorGradeParams`, exposure/saturation/CDL stub |
| `postprocess/post_stack.hpp` | `PostStack` facade chaining the stages |

## Tests (`ctest`)

| Test | Coverage |
|------|----------|
| `fuse_ddgi` | Grid math, probe indexing, atlas layout, trilinear irradiance lerp, hysteresis blend, probe scheduling, init/update/sample, pipeline slot |
| `fuse_post_process_b510` | Bloom thresholding, ACES clamping, neutral 0.18 grey calibration, tonemap curve rolloff/Reinhard+ACES clamp, EMA convergence, empty histogram, auto-exposure EV metering/adaptation, stage ordering, `PostStack` facade |
