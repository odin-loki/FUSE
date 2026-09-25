# Upscalers

FUSE has one upscaler abstraction with swappable backends. Temporal backends (native TAAU, FSR 3.1 on Vulkan,
DLSS through the NVIDIA plugin; XeSS later) and spatial backends (FSR 1, NVIDIA Image Scaling) share one
registry and one set of capability queries. The CAS sharpener is registered as a 1x backend and can also run
on its own in the post chain. The design follows `docs/research/upscaling-framegen-and-post-injectors.md` (§4.1
and recommendation 3 for the order of backends, §6 for licensing).

| File | Contents |
| --- | --- |
| `Renderer/include/fuse/renderer/upscale/upscaler.hpp` | `IUpscaler`, `ISpatialUpscaler`, `ITemporalUpscaler`, `UpscalerCaps`, `UpscalerRegistry`, `IJitterProvider` / `HaltonJitterProvider`, quality modes, history reset reasons |
| `Renderer/include/fuse/renderer/upscale/upscale_inputs.hpp` | The canonical temporal input contract `UpscaleInputs`, owned by the TAAU work stream. `upscaler.hpp` aliases it. |
| `Renderer/include/fuse/renderer/upscale/upscale_passes.hpp` | Host entry points `run_easu`, `run_rcas`, `run_cas`, `run_nis` and `run_bilinear`, plus constant setup that matches the SDK host helpers |
| `Renderer/include/fuse/renderer/upscale/{fsr1,cas,nis}_kernel.hpp` | Single-source `FUSE_HOST_DEVICE` ports of the vendored shaders (see `docs/compute-kernels.md`) |
| `Renderer/src/upscale/upscaler_backends.cpp` | The built-in backends: `native_taau`, `fsr1`, `nis` and `cas` |
| `Renderer/cmake/upscale.cmake` | Sources, build-time SPIR-V of the vendored GLSL, gate tests and the `FUSE_UPSCALER_FSR3` option (FSR 3.1 registration) |
| `Renderer/{include/fuse/renderer,src}/upscale_backends/fsr3/`, `Renderer/cmake/rp_wp42.cmake` | The FSR 3.1 backend (see "FSR 3.1" below) |

## Interface

```cpp
namespace fuse::renderer::upscale {
class IUpscaler         { caps(); render_size(display, mode); supports(render, display); };
class ISpatialUpscaler  : IUpscaler { evaluate(dispatch, SpatialUpscaleInputs, UpscaleOutputs /*RGBA*/); };
class ITemporalUpscaler : IUpscaler { evaluate(dispatch, UpscaleInputs, TemporalUpscaleOutputs /*linear RGB*/);
                                      jitter_provider(); invalidate_history(reason); history_generation();
                                      last_reset_reason(); accumulated_frames(); };
}
```

- **Quality modes** (`QualityMode` = `renderer::UpscaleQualityMode`): NativeAA 1.0x, UltraQuality 1.3x,
  Quality 1.5x, Balanced 1.7x, Performance 2.0x and UltraPerformance 3.0x, per axis. `render_extent()` rounds
  the result, so 1080p gives 1280x720 at Quality, 1129x635 at Balanced and 640x360 at UltraPerformance.
  `mip_lod_bias()` returns `log2(render/display) - 1` for temporal backends and `log2(render/display)` for
  spatial ones.
- **Capabilities** (`UpscalerCaps`) report:
  - the kind: temporal, spatial or sharpen;
  - whether the backend needs depth, motion vectors, jitter or exposure;
  - whether it needs or accepts reactive and transparency masks;
  - whether it wants HDR (pre-tonemap) or LDR input, and whether it has built-in sharpening;
  - the supported ratio range and quality-mode bits;
  - the APIs compiled in (`Cpu`, `Vulkan`, `Cuda`);
  - the licence and a `stub` flag.
- **Registry**: `UpscalerRegistry::instance()` holds the built-in backends in preference order.
  - `select(UpscalerRequirements)` returns the first backend that matches the mode, the API and the inputs the
    renderer has. For example, a frame without motion vectors falls back to a spatial backend. Stub backends
    are skipped unless `allow_stub` is set.
  - `register_backend` / `unregister_backend` let optional plugins add backends at runtime. The NVIDIA plugin
    registers `dlss_sr` and `dlss_rr` this way.
- **Jitter**: `HaltonJitterProvider` gives Halton(2,3) offsets over `ceil(8 * ratio^2)` phases, the same as
  `ffxFsr3GetJitterPhaseCount` / `ffxFsr3GetJitterOffset`. The offsets are in render pixels with +y down:
  render pixel (i, j) samples the unjittered scene at (i + 0.5 + jx, j + 0.5 + jy), so the jittered
  projection moves every point by -jitter. `offset_ndc()` (= `upscaleJitterNdc`) converts them for a Vulkan
  projection (NDC y down): (-2 jx / w, -2 jy / h), the same matrix as `temporal::jitter_view_proj`. Every
  temporal backend (native TAAU, WP-4.1 GPU TAAU, FSR 3.1) uses this one convention; the gate
  `fuse_rp_fsr3_jitter` pins them against each other. (Before the WP-4.2 follow-up the x term was +2 jx / w,
  which moved points by +jitter in x.)
- **History**: temporal backends drop history in three cases, each with a `HistoryResetReason` and a bumped
  `history_generation()`:
  - a resolution or ratio change (`ResolutionChange`);
  - `UpscaleInputs::reset_history` (`CameraCut`);
  - an explicit `invalidate_history(reason)` call.

### Pipeline placement

- **Temporal backends** replace TAA. They run before bloom, DoF, motion blur, tonemap, grade and grain, on
  linear HDR input, and output linear display-resolution RGB.
- **FSR 1 and NIS** run after tonemap and before grain and UI, on display-referred [0,1] input. So does CAS
  when it sharpens the post chain.

## Built-in backends

| id | kind | ratio | modes | inputs | notes |
| --- | --- | --- | --- | --- | --- |
| `native_taau` | temporal | 1x to 3x | all | colour, depth, motion, exposure; reactive and T&C masks optional | Adapter over `renderer::TaauUpscaler` (`taa/taau.hpp`, single-source kernel `taau`). |
| `fsr1` | spatial | 1x to 4x | UltraQuality to UltraPerformance | LDR colour | EASU into an intermediate, then RCAS (`sharpness` in [0,1] maps to `2 * (1 - s)` RCAS stops). This is the SDK's `FFX_FSR1_OPTION_APPLY_RCAS=1` pipeline. |
| `nis` | spatial | 1x to 2x | UltraQuality to Performance | LDR colour | NVScaler, a 6-tap 64-phase scaler with directional USM sharpening. `NVScalerUpdateConfig` rejects scale factors below 0.5. |
| `cas` | sharpen | 1x | NativeAA | LDR colour | CAS sharpen-only. Also available directly as `run_cas()`. |

### How the CPU ports are made

The vendored shaders are GLSL/HLSL. Each port transcribes the shader's float32 path operation for operation
into a `FUSE_HOST_DEVICE` item kernel with one output pixel per item and 8x8 workgroups:

- **Approximations.** FidelityFX's bit-trick approximations (`ffxApproximateReciprocal`, `...Medium`,
  `...ReciprocalSquareRoot`, `ffxApproximateSqrt`) are reproduced bit for bit.
- **min/max.** GPU min/max are IEEE minNum/maxNum, so a NaN operand yields the other operand. RCAS relies on
  this for black neighbourhoods, where it computes 0 * inf.
- **Constants.** `easu_constants`, `rcas_constants` and `cas_constants` produce the same u32 bit patterns as
  `ffxFsrPopulateEasuConstants`, `FsrRcasCon` and `ffxCasSetup`. The gate checks this bitwise against the
  vendored headers compiled with `FFX_CPU`. NIS uses the vendored `NVScalerUpdateConfig` and its coefficient
  banks directly.
- **NIS tiling.** NIS stages luma, an edge map and filter banks in groupshared memory. Every staged value
  depends only on absolute source coordinates, so the port evaluates them per pixel. It is the same
  computation without the tile.
- **Edges.** EASU gathers and NIS loads go through a linear-clamp sampler, so the ports clamp to the edge.
  RCAS and CAS use `texelFetch`, which is undefined outside the image on the GPU. The ports define it as
  clamp-to-edge, and the GPU comparison skips the 1-pixel border for those two passes.
- **Not ported:**
  - the FP16 paths;
  - CAS's own scaling mode (`casFilterWithScaling`);
  - NIS HDR modes, NV12 input and NVSharpen.

## Vendored third-party code

Both libraries are MIT-licensed and vendored verbatim with no local edits. Each is pinned in a `VERSION` file.
`fuse_lint vendored-pins` checks every pin (ctest `fuse_lint_vendored_pins_fidelityfx` and
`fuse_lint_vendored_pins_nvidia_nis`), and every file's SHA-256 is recorded.

| Directory | Upstream | Tag / commit | Licence | Contents |
| --- | --- | --- | --- | --- |
| `Engine/lib/fidelityfx` | GPUOpen-LibrariesAndSDKs/FidelityFX-SDK | v1.1.4 / `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` | MIT, Copyright (C) 2024 Advanced Micro Devices, Inc. (`LICENSE.txt`) | `gpu/ffx_core*.h`, `ffx_common_types.h`, `gpu/fsr1/*` and `gpu/cas/*` (FSR1 and CAS 1.2.0, `header_version`), `host/ffx_fsr1.h` and `host/ffx_cas.h` (version declarations only; the host runtime is not compiled), and `shaders/vk/{fsr1,cas}/*.glsl` (Vulkan pass entry points) |
| `Engine/lib/nvidia-nis` | NVIDIAGameWorks/NVIDIAImageScaling | v1.0.3 / `35e13ba316c98eeecf16f37eae70ce88019911f6` | MIT, Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES (`licence.txt`) | `NIS/NIS_Scaler.h`, `NIS_Config.h` (constants and coefficient tables), `NIS_Main.glsl` and `NIS_Main.hlsl` |

`fuse_lint vendored-pins` accepts `#define <X>_VERSION_MAJOR/MINOR/PATCH` triplets (FidelityFX) and an
`SDK - vX.Y.Z` banner (NIS) as version declarations. It also accepts a `header_version=` key for a component
whose version differs from the SDK tag.

## Gates

| ctest | What it checks |
| --- | --- |
| `fuse_upscale_gates` (label `gate`) | Quality modes and render extents; caps; `select()` preference and fallbacks; register, unregister and create; spatial and temporal validation; jitter phase counts, offsets, periodicity and NDC conversion; `native_taau` history generation and reset reasons (resolution change, camera cut, explicit); `fsr1` equals `run_easu` + `run_rcas` bitwise; SDK constant setup matches bitwise; CpuReference equals CpuParallel bit-exactly at 0, 2 and 4 workers for EASU, RCAS, CAS (two variants), NIS and bilinear on grids with partial 8x8 tiles; kernel stats names and item counts; a Cuda request without a device falls back to CpuParallel with identical output; quality against bilinear (below). |
| `fuse_upscale_gpu_reference` (labels `gate;vulkan`, SKIP 77 without a loader, device or shaders) | Runs the vendored GLSL passes on a Vulkan device (Lavapipe in CI) and compares them with the CPU ports. The passes are compiled by `glslangValidator` at build time (`build/.../shaders/upscale/*.comp.spv`, validated with `spirv-val`). See the tolerances below. The harness opens `libvulkan` at runtime, so the gate also runs in stub builds. |
| `fuse_lint_vendored_pins_fidelityfx`, `fuse_lint_vendored_pins_nvidia_nis` | Pin files, licence, declared version and per-file SHA-256. |

Reports are written to the build tree:

- `Source/FUSE/Renderer/upscale_quality_report.json`: PSNR, SSIM and FLIP per backend and ratio, plus kernel stats.
- `Source/FUSE/Renderer/upscale_gpu_reference_report.json`: max and mean error per GPU case, plus the device name.

### Results (stub build, Lavapipe, 2026-09-23)

**Quality against bilinear** (`fuse_upscale_gates`). The ground truth is the analytic scene area-averaged at
384x256 display resolution. The input is the same scene area-averaged at render resolution.
`native_taau` accumulates one full jitter cycle (18, 24, 32 or 72 frames) on a static camera. `fsr1` uses
sharpness 0.2 (1.6 RCAS stops). `nis` uses sharpness 0.5. The metrics come from `quality/image_metrics.hpp`.
For PSNR and SSIM higher is better; for FLIP lower is better.

| ratio | bilinear PSNR / SSIM / FLIP | EASU | FSR1 (EASU+RCAS) | NIS | native_taau |
| --- | --- | --- | --- | --- | --- |
| 1.5x | 28.02 / 0.9609 / 0.0327 | 29.87 / 0.9737 / 0.0222 | 30.01 / 0.9751 / 0.0293 | 29.75 / 0.9738 / 0.0272 | 31.27 / 0.9815 / 0.0251 |
| 1.7x | 26.90 / 0.9475 / 0.0392 | 28.73 / 0.9632 / 0.0276 | 28.85 / 0.9647 / 0.0341 | 28.51 / 0.9651 / 0.0339 | 30.22 / 0.9759 / 0.0275 |
| 2.0x | 25.98 / 0.9354 / 0.0462 | 28.19 / 0.9570 / 0.0305 | 28.31 / 0.9584 / 0.0369 | 27.60 / 0.9550 / 0.0398 | 28.76 / 0.9656 / 0.0321 |
| 3.0x | 23.33 / 0.8914 / 0.0705 | 24.48 / 0.9132 / 0.0538 | 24.51 / 0.9138 / 0.0587 | n/a (max 2x) | 25.57 / 0.9277 / 0.0488 |

The gate checks four things:

- EASU, FSR1 and NIS each beat bilinear on all three metrics at every supported ratio.
- `native_taau` beats the best spatial backend on PSNR and SSIM.
- CAS on a softened (3x3 tent) copy of the ground truth raises PSNR and SSIM. At sharpness 0.5 the result
  goes from 28.21 dB / 0.9606 to 29.80 dB / 0.9745; at sharpness 1.0 it reaches 30.50 dB / 0.9780.
- CpuReference equals CpuParallel bit-exactly for every kernel at 0, 2 and 4 workers.

**CPU ports against the vendored GLSL on Lavapipe** (`fuse_upscale_gpu_reference`, llvmpipe with LLVM 20.1.2):

| pass | cases | max abs error | mean abs error |
| --- | --- | --- | --- |
| EASU | 128x96 to 192x144, 113x64 to 192x108, 96x72 to 192x144, 64x48 to 192x144, 61x43 to 97x71 | ≤ 4.2e-7 | ≤ 1.9e-8 |
| RCAS | 97x71, 192x144, 150x100 at 0, 0.2 and 1.6 stops | ≤ 3.0e-7 | ≤ 1.8e-8 |
| CAS | 97x71, 160x120, 131x77 at sharpness 0, 0.5 and 1 (after unorm16 rounding) | 1.5e-5 (1 LSB of unorm16) | ≤ 2.0e-8 |
| NIS, power-of-two sources | 128x64 at 1x, 1.5x and 1.7x; 64x64 at 2x; 64x32 to 97x61 | ≤ 2.4e-7 (exactly 0 at 1x) | ≤ 1.6e-8 |
| NIS, non-power-of-two sources | 96x72 to 192x144, 61x43 to 110x80 | 0.12 on 1.8% and 4.4% of values | 2.6e-4 and 7.7e-4 (bound 2e-3) |

**Why NIS needs a separate bound for non-power-of-two sources.** The shader loads texels through a linear
sampler at `(x + 0.5) * (1 / width)`. When `1 / width` is not exact, the sampler's sub-texel quantization
(`subTexelPrecisionBits` is 8 on Lavapipe) blends up to 1/256 of a neighbouring texel into what should be an
exact load. NIS's edge thresholds and its sharpening then amplify that difference. This comes from the
sampler hardware, not the algorithm: the same inputs at power-of-two sizes agree to 2.4e-7.

**NIS v1.0.3 top/left border behaviour.** The ports reproduce this, and the gate caught it. The tile loader
computes `srcBlockStartX + px` as int + uint, which GLSL and HLSL evaluate as uint. When upscaling, the first
block starts at -1, so the first 2x2 load batch wraps to about 4.3e9 and the sampler clamps it to the far
edge. As a result, source rows and columns -3 and -2 read the last row or column of the image instead of the
first. `nis_pixel` reproduces this so the CPU and GPU outputs stay identical.

## FSR 3.1 (WP-4.2, Vulkan)

FSR 3.1 is vendored and built: the AMD FidelityFX SDK v1.1.4 (`c6efa6bf`, MIT, FSR 3.1 upscaler 3.1.4) subset
in `Engine/lib/fidelityfx` (`gpu/fsr3upscaler/*.h`, `gpu/spd/ffx_spd.h`, the 8 Vulkan GLSL passes), pinned by
SHA-256 in its `VERSION` file (lint `fuse_lint_vendored_pins_fidelityfx`). No SDK host runtime, FidelityFX
Vulkan backend, FidelityFX-SC or frame generation is vendored.

| Piece | Where |
| --- | --- |
| Build (lib `fuse_fsr3`, passes compiled with `glslangValidator -V -Os` for one permutation and embedded, gates) | `Renderer/cmake/rp_wp42.cmake` |
| Clean-room host port of `ffx_fsr3upscaler.cpp`, `Fsr3Gpu` (render graph v2), the `fsr3.convert` input adapter | `Renderer/{include/fuse/renderer,src}/upscale_backends/fsr3/` |
| `"fsr3"` `ITemporalUpscaler` adapter (`Fsr3TemporalUpscaler`) and `register_fsr3_backend()` | `upscale_backends/fsr3/fsr3_upscaler.hpp` |

**Registration.** `"fsr3"` needs a Vulkan device, so it is not a built-in: the renderer calls
`fsr3::register_fsr3_backend(registry, {device, allocator, executor})`, which registers it (caps: temporal,
Vulkan, MIT, ratio 1 to 3, RCAS, dynamic resolution) when the device can run the passes.

**`FUSE_UPSCALER_FSR3`** (CMake option in `upscale.cmake`, default ON when the Vulkan backend is on, OFF in
stub builds) controls that registration: with it OFF, `register_fsr3_backend()` always returns false and
`"fsr3"` is never registered (the `fuse_rp_fsr3_vk_switch_set` gate then skips). `fuse_fsr3`, `Fsr3Gpu` and
the CPU gates build either way. Trees configured before FSR 3 was vendored had a forced OFF cached; that entry
is dropped once so the new default applies.

**Conventions** (`fsr3_types.hpp`): FSR `jitterOffset = -jitter_px` (the SDK's `Jitter()` is the content
displacement, FUSE's `jitter_px` the sample offset); the projection is jittered with `IJitterProvider::offset_ndc`
/ `upscaleJitterNdc` like every other temporal backend; UV motion is passed unchanged with
`fMotionVectorScale = -1`; linear depth is converted to reverse-Z device depth by `fsr3.convert`.

Gates, measured quality and open items: the WP-4.2 row of `docs/unification/RENDERER-EXECUTION.md`.
