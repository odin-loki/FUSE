# FUSE Renderer — Hardware Exit-Criteria Run

**Device:** NVIDIA GeForce RTX 3090 (24 GB, driver 610.88, Windows 11, native MSVC Release build). It is the first real T2/T3 device.
**Date:** 2026-11-05.
**Scope:** the rows that [RENDERER-EXECUTION.md](./RENDERER-EXECUTION.md) tags **LVP-func / HW**, measured against the numbers in [FUSE_RENDERER_PLAN.md](../plans/FUSE_RENDERER_PLAN.md) (§5.3, the P2 / P3 / P5 / P7 / P8 / P9 exits, and §8).
**Code:** `Source/FUSE/Renderer/cmake/rp_hw_bench.cmake` and `Source/FUSE/Renderer/tests/test_rp_hw_bench*.cpp` (+ `_common.hpp`, `_scene.hpp`). Baselines and the asset manifest live in `Source/FUSE/Renderer/tests/perf/`.

RENDERER-EXECUTION.md is owned by other agents, so this run does not edit it. Its owners should copy the verdicts below into the P2 / P3 / P5 / P7 / P8 / P9 / §8 rows.

---

## 1. Summary

The P2, P5 and P8 timings come from one run of each ctest after the GPU went idle. Their trial spread is given in the sections below. The P7 and P9 rows come from separate runs of their own benches (§6 and §7).

| Exit criterion | Plan number | Measured on the RTX 3090 | Verdict |
|---|---|---|---|
| §5.3 frame at 1440p (Sponza stand-in, T2 preset) | 16.6 ms total | **15.7 ms** in sum of passes (16.0 ms span) | total **within**. Per stage: 3 within, 3 over (§3) |
| §5.3 frame at 1440p (Bistro stand-in, 3.9M tris, 256 lights) | 16.6 ms total | **16.5 ms** (16.8 ms span) | total at the limit. Per stage: 2 within, 4 over |
| §5.3 frame, `FrameSettings{}` as shipped (Sponza) | 16.6 ms | **75.7 ms**, of which SSGI is 55 ms | **over** (fixed by the preset, §3.1) |
| P2: 4,096 lights within the lighting budget | 3.0 ms direct lighting + shadows | **8.97 ms**, of which `light.shade` is 5.9 ms and `light.cull` 0.8 ms. With 256 lights: 4.76 ms | **miss** (§4) |
| P3: VSM cached frame | < 1 ms | **0.25 ms** (Sponza), **0.22 ms** (Bistro). With a moving camera: 0.28 / 0.26 ms | **pass** |
| P5: > 100M source triangles within budget | 2.0 ms culling + visibility | **104.9M** source triangles: GPU LOD cut 1.27 ms + cull / visbuffer of the 0.34M-triangle cut 0.57 ms = **1.85 ms** (3 runs: 1.844–1.852) | **pass**, with a caveat (§5) |
| P7: 10k+ emissive lights, stable and low noise | qualitative | 10,000 emissive triangles. relMSE against a 4096-spp f64 reference: RIS 0.176, ReSTIR (unbiased) 0.0144, + SVGF **8.1e-4**. Frame-to-frame change 0.35 % | quality **pass**. Cost **miss**: `restir.di.initial` takes 145 ms at 1440p (§6) |
| P8: volumetrics ≤ 1.5 ms | 1.5 ms | fog + clouds **1.20 ms** (Sponza), **1.31 ms** (Bistro). The volumetrics + transparency stage is 1.42 / 1.64 ms | **pass**. The stage including transparency is 0.14 ms over on Bistro. With 4,096 lights, `fog.inject` reaches 2.97 ms |
| P9: measured against the conventional path | win or do not ship | NTC loses on quality and cost. Gaussian splats lose on memory and quality, and tie or lose on cost (§7) | **neither ships** as a replacement |
| §8 Sponza / Bistro | reference scenes | harness, pinned-hash manifest and fetch script are in place. **Not downloaded** (needs the user's permission) | pending the fetch (§8) |

## 2. Harness and noise strategy

- **Timing source.** WP-0.6 `GpuProfiler` timestamp zones. The `rg::Executor` pass hooks put one zone around every executed render-graph pass. A stage time is the sum of its passes in one frame. "Total" is the sum of every pass. "Span" runs from the first zone's begin to the last zone's end, so it includes the gaps between passes.
- **Frame.** One `FrameComposer` frame per graph, at T2:
  - render 1706×960, then TAAU to a 2560×1440 output;
  - TLAS, RT shadows + SVGF, ray-query DDGI, VSM, clustered lighting, SSFX, sky and aerial perspective, froxel fog, clouds, forward transparency, RT reflections + SVGF, and the post stack;
  - ReSTIR DI and frame generation off. ReSTIR is measured on its own in §6.
- **Stage mapping.** `stageOf()` in `test_rp_hw_bench.cpp` maps passes to the §5.3 stages:

  | §5.3 stage | Passes |
  |---|---|
  | Culling and visibility buffer | `gpu_scene.*`, `cull.*`, `vis.*`, and TLAS/BLAS builds |
  | Material resolve | `resolve.*`, `temporal.motion` |
  | Direct lighting and shadows | `light.*`, `vsm.*`, `rt.shadows`, the shadow SVGF, `frame.shadow_pack` |
  | Global illumination | `ddgi.*`, `ssfx.*`, `rt.reflections`, the reflection SVGF, `frame.reflect` |
  | Volumetrics and transparency | `fog.*`, `clouds.*`, `atmos.*`, `frame.sky` / `.aerial` / `.gather`, `forward.*` |
  | Post-process and upscale | `taau.*`, `post.*`, `frame.resolve` |

  Every composer SVGF instance names its passes `denoise.*`. The harness tells them apart by recording order: denoise passes after `rt.reflections` become `refl/denoise.*`.
- **Noise.**
  - 90 warm-up frames are discarded. They cover clocks, pipeline caches, temporal histories and the VSM page cache.
  - Then K = 5 trials of N = 48 frames, with 8 re-warm frames between trials.
  - The reported number is the median of the 5 per-trial medians, with the [min..max] trial median as the spread.
  - Every frame waits idle (`executor->waitIdle`), so timestamps never overlap the next frame.
  - Frames must be heavy enough to keep the GPU at boost clocks. A 16 ms composer frame is. Tiny stand-alone frames are not: see the P5 note in §5.
- **Sharing the GPU.** Six other agents shared the GPU. A CUDA parity test from another agent kept it at 100 % for a while, and the first numbers under that load were inflated 1.5–3× (`ssfx.gtao` showed 13.8 ms instead of 2.5 ms). All numbers here were taken with `nvidia-smi` at 0–2 % before and after each block. On a quiet GPU the trial spread is ±1–2 % within a run, and run to run it is about 2.5 % (Sponza total 15.74 vs 16.11 ms).
- **Regression gate.** `fuse_rp_hw_budget_1440p_{sponza,bistro}` (labels `perf;hw`, RUN_SERIAL, exit 77 without a discrete GPU or without a baseline entry for the device).
  - It compares every §5.3 stage and the total with `tests/perf/rp_hw_bench_baseline.json`. Each device has its own entry, keyed `<device name> | <scene>`.
  - A value fails above `baseline × 1.05 + 0.02 ms`. The plan §8 asks for failure above a 5 % regression. The 0.02 ms floor keeps stages under about 0.4 ms from failing on timestamp granularity.
  - The gate re-measures once before failing, so one slow trial caused by a foreign process does not fail the run.
  - `FUSE_UPDATE_PERF_BASELINE=1` rewrites the measured device's entry.

## 3. §5.3 per-pass budget at 1440p

### 3.1 Preset: the optimisation that was feasible here

`FrameSettings{}` as shipped is a reference-quality configuration. On the Sponza stand-in it costs **75.7 ms**:

| Pass | Cost | Cause |
|---|---|---|
| `ssfx.ssgi` | 55.0 ms | 16 rays × 64 steps per pixel |
| `ssfx.gtao` | 6.0 ms | 4 slices × 8 steps |
| `denoise.atrous` | 4.65 ms | 5 iterations each for the shadow and reflection signals |

The harness therefore measures a **T2 1440p preset** (`applyPreset`):

- **SSGI off.** At T2, ray-query DDGI supplies the indirect diffuse. Plan 6c names SSR and GTAO, not SSGI, as the screen-space supplement, and RT reflections already replace SSR.
- **GTAO** at 2 slices × 6 steps.
- **RT-shadow SVGF** at 3 à-trous iterations instead of 5.

Together these remove 59.6 ms. `--preset defaults` measures the shipped settings.

**Recommendation for the composer owners:** make this the default T2 preset. In particular, do not run 16-ray SSGI next to DDGI.

### 3.2 Results with the preset

The table gives the median of trial medians in ms, with [min..max] across trials.

| Stage | Budget | Sponza stand-in (0.55M tris, 244 inst., 9 lights) | Bistro stand-in (3.92M tris, 2,748 inst., 257 lights) |
|---|---|---|---|
| Culling and visibility buffer | 2.0 | 0.239 [0.237..0.243] | 0.604 [0.594..0.609] |
| Material resolve | 1.5 | 1.055 [1.044..1.075] | 1.015 [0.991..1.024] |
| Direct lighting and shadows | 3.0 | **4.589** [4.543..4.661] | **4.759** [4.666..4.811] |
| Global illumination | 3.0 | **6.213** [6.150..6.308] | **6.251** [6.141..6.295] |
| Volumetrics and transparency | 1.5 | 1.322 [1.316..1.343] | **1.565** [1.552..1.577] |
| Post-process and upscale | 2.0 | **2.327** [2.313..2.374] | **2.354** [2.319..2.383] |
| UI, headroom | 3.6 | 0 (no UI source) | 0 |
| **Total** | **16.6** | **15.737** [15.595..15.981], span 16.02 | **16.546** [16.313..16.711], span 16.83 |

### 3.3 Budget misses and where the time goes

The costs below are per-pass medians, Bistro first.

**Direct lighting and shadows (4.6–4.8 ms, budget 3.0)**
- `light.shade` costs 2.5–2.6 ms, roughly the same with 9 or 257 lights, so most of it is a fixed per-pixel cost. Each pixel does five G-buffer fetches, `fuseLcShadowedPx`, a DDGI 8-probe sample with visibility, and the multi-scatter BRDF LUT.
- `fuseLcShadowedPx` evaluates the shadow lookup even for lights whose contribution is zero. The kernel is compiled with `-fp-mode precise` for bit parity with the CPU oracle.
- These are the first two places to look. The kernel belongs to WP-2.1 / 2.2 and was not changed here.
- The shadow SVGF costs 1.6 ms at 3 iterations.

**Global illumination (6.2 ms, budget 3.0)**

| Pass | Cost |
|---|---|
| `ssfx.gtao` | 2.5 ms at 2 × 6 samples, about 30× what a GTAO pass at this resolution usually costs. The kernel is a per-pixel bindless-fetch loop without shared memory and should be rewritten. |
| Reflection SVGF | 2.7 ms total: `refl/denoise.atrous` 2.25 ms at a fixed 5 iterations (not configurable through `DenoiserSettings`), plus temporal and variance |
| `rt.reflections` | 0.36–0.45 ms |
| DDGI | 0.2 ms |

**Post-process and upscale (2.35 ms, budget 2.0)**
- `taau.resolve` costs 2.05–2.1 ms at the 2560×1440 output.

**Volumetrics and transparency on Bistro (1.57 ms, budget 1.5)**
- `forward.draw` grows with the glass store fronts, and `fog.inject` grows with the clustered lights.
- Fog and clouds alone stay within budget (§4).

**Within budget**
- Culling + visibility buffer: 0.24–0.60 ms.
- Material resolve: about 1.0 ms. `resolve.gbuffer` takes 0.75–0.8 ms of that.
- VSM: 0.22–0.25 ms in total.

## 4. Plan budget numbers: P2, P3, P8

These modes run on the same harness, with the same preset and noise strategy.

### P2: 4,096 lights within the lighting budget (`fuse_rp_hw_p2_lights_4096`, fails)

Bistro stand-in with 4,096 local lights (3,072 point and 1,024 spot, range 3–7 m) plus the sun. Plan budget for direct lighting and shadows: 3.0 ms.

| Lights | Direct lighting + shadows | `light.*` | `light.shade` | `light.cull` | `vsm.*` | RT shadows + SVGF + pack | `fog.inject` |
|---|---|---|---|---|---|---|---|
| 256 | 4.756 [4.730..4.823] | 2.670 | 2.57 | 0.05 | 0.219 | 1.893 | 0.39 |
| 4,096 | **8.972** [8.895..9.031] | 6.846 | 5.94 | 0.80 | 0.220 | 1.902 | 2.97 |

- **Miss by 3×.** Even `light.*` on its own is 6.8 ms, more than twice the whole stage budget.
- **Scaling.** Going from 256 to 4,096 lights adds 3.4 ms to `light.shade` and 0.75 ms to `light.cull`. The clusters use 16×9×24 tiles with at most 256 lights per cluster.
- **Fog.** The froxel fog's clustered light loop (`fog.inject`) grows from 0.39 to 2.97 ms. That pushes "volumetrics + transparency" to 4.45 ms.
- **Fixed cost.** Of the 4.76 ms at 256 lights, 2.5 ms of `light.shade` is fixed per pixel (§3.3).
- **Suggested fixes:**
  - early-out on zero contribution before the shadow lookup;
  - a light-count-aware tile loop with wave-uniform light fetch;
  - a coarser cluster grid for the fog (it does not need 256 per cluster);
  - relaxing `-fp-mode precise` for the production permutation, with parity kept on the reference permutation.

### P3: VSM cached frame (`fuse_rp_hw_p3_vsm_cached`, passes)

Static camera and static scene, after 90 warm-up frames. The number is the sum of all `vsm.*` passes.

| Scene | Cached (static) | Moving camera (0.05 m / frame) | Largest passes |
|---|---|---|---|
| Sponza | **0.249 ms** | 0.278 ms | `vsm.mark` 0.11, `vsm.alloc` 0.11 |
| Bistro | **0.223 ms** | 0.256 ms | the same; `vsm.raster` 0.002 ms when cached |

Pass. The plan asks for under 1 ms. The pool is 64×64 pages, with 16×8 local pages.

### P8: volumetrics ≤ 1.5 ms (`fuse_rp_hw_p8_volumetrics`, passes)

Moving camera, so the fog reprojection is active. Froxel grid 160×90×64 (the defaults), clouds at the render resolution.

| Scene | fog + clouds + `frame.gather` | Atmosphere LUTs + sky + aerial | §5.3 volumetrics + transparency stage |
|---|---|---|---|
| Sponza | **1.201 ms** | 0.183 ms | 1.415 ms |
| Bistro | **1.308 ms** | 0.183 ms | 1.636 ms |

- **Bistro passes:**

  | Pass | ms |
  |---|---|
  | `clouds.march` | 0.49 |
  | `fog.inject` | 0.43 |
  | `fog.integrate` | 0.13 |
  | `fog.temporal` | 0.07 |
  | `clouds.composite` | 0.07 |
  | `fog.apply` | 0.06 |
  | `frame.gather` | 0.05 |

- **Pass.** One caveat: `fog.inject` scales with the clustered light count, reaching 2.97 ms at 4,096 lights (P2).
- **Ghosting.** The criterion's "no temporal ghosting" half is covered on Lavapipe by `fuse_rp_volumetric_gpu_vk_ghosting_set`.

## 5. P5: > 100M-triangle DAG scene (`fuse_rp_hw_p5_dag_100m`, passes)

**Scene.**
- One heightfield tile: 640×640 quads = 819,200 triangles.
- It goes through the WP-5.2 DAG (17,996 clusters, 8,483 leaves, 1,109 groups, 14 levels; 3.1 s to build) into 1,043 WP-5.3 pages of 32 KiB.
- The tile is instanced 16×8 = 128 times, for **104,857,600 source triangles**.
- Camera 3 m above the near edge, looking across 512 m of terrain; 2560×1440, 1 px error threshold.

**Results.**

| Part | Time (ms) | Notes |
|---|---|---|
| GPU LOD cut: `stream.state` + `stream.cut` + `stream.feedback`, one GeometryStreaming per instance, 384 passes, recorded in the same graph as the draw | **1.272–1.276** [1.268..1.277] | The residency settles after 18 frames at 592 resident pages |
| Cull + visibility buffer of the selected cut, at native 1440p | **0.569–0.579** | The cut is 3,966 clusters, **339,822 triangles** (0.32 % of the source) |
| **Culling + visibility total** | **1.844–1.852** over 3 runs | Budget 2.0: **within** (7 % margin) |
| Material resolve of the cut, at native 1440p | 1.571–1.576 | Reported only. The §5.3 frame resolves at 1706×960 for about 1.0 ms |

**Idle clocks.** The first version timed the stream passes in their own stream-only frames. The 3090 then dropped to idle clocks (210 MHz) between those tiny frames, and the cut read anywhere from 1.3 to 10.2 ms on an otherwise idle GPU. The ctest failed once that way. The bench now records the cut in the same graph as the draw frame, which is also the realistic placement, and reports the stream-only number only as a diagnostic (1.4–2.1 ms).

**Caveats.**
- **The DAG clusters are not drawn directly.** The WP-5.1 mesh path does not draw DAG clusters yet (RENDERER-EXECUTION.md P5 row). The same views' fully-resident CPU cut (`dag::select_dag_cut`, whose parity with the GPU cut is gated on Lavapipe) is decoded into one meshlet mesh per tile and drawn through cull + visbuffer. The raster cost is therefore that of the exact cut geometry, but not through a page-pool draw path.
- **The cut cost is mostly per-pass overhead.** 128 × 3 small dispatches, each with its own barriers. A single instanced `stream.cut` dispatch would bring 1.29 ms down to about the cost of one tile's cut, and is the obvious follow-up.

## 6. P7: ReSTIR DI with 10k emissive lights (`fuse_rp_hw_p7_restir_10k`)

**Scene.** The WP-7.2 test scene: 10,000 emissive triangles in the WP-7.1 light tree, a static camera, 320×240, DI only, Slang kernels, production `RestirSettings` (8 candidates, 1 spatial iteration × 3 neighbours).

**Reference.** `RestirReference::direct` in f64 at **4096 spp per pixel**, evaluated at the GPU's read-back surfaces, over 66,697 valid pixels. It takes 2.0–2.1 s on 64 threads, and its own noise adds 2.0e-4 to every relMSE.

**Metric.** relMSE = mean of (L − L_ref)² / (L_ref² + (0.1·mean L_ref)²) on luminance.
- RIS-only: averaged over 64 independent frames.
- ReSTIR: averaged over 16 frames, after 32 frames of convergence.

| Configuration | relMSE | Bias | Frame-to-frame change |
|---|---|---|---|
| RIS-only (8 candidates, reset every frame) | 1.76e-1 | +0.00 % | 35.9 % |
| ReSTIR DI, biased (default) | 5.84e-2 | −6.44 % | 9.80 % |
| ReSTIR DI, biased + SVGF | 1.63e-2 | −5.51 % | 0.44 % |
| ReSTIR DI, unbiased | 1.44e-2 | +0.57 % | 7.74 % |
| ReSTIR DI, unbiased + SVGF | **8.07e-4** | +0.36 % | **0.35 %** |

**Equivalent RIS-only frames.** Averaging N RIS-only frames gives relMSE ≈ 0.176 / N. One ReSTIR frame is worth:
- about 3 averaged RIS frames (biased);
- about 12 (unbiased);
- about 11 (biased + SVGF);
- more than 64 (unbiased + SVGF).

**Quality verdict.** The exit's quality criterion ("stable, low-noise output after denoising") **passes**. Unbiased + SVGF lies within 0.4 % of the reference, with relMSE 8e-4 and 0.35 % frame-to-frame change.

**The biased default loses energy.** It sits about 6 % dark. **Recommendation:** make the unbiased mode the default. It costs 10 ms more at 1440p today, but that cost is far below the problem below.

**Cost at 2560×1440 (a real miss).** 30 warm-up frames, then 3 trials × 100 frames, with the GPU idle.

| Configuration | `restir.*` | SVGF |
|---|---|---|
| RIS-only | 146.1 ms [145.0..147.4] | — |
| Biased + SVGF | 155.7 ms [155.5..156.9] | 5.42 ms |
| Unbiased + SVGF | 166.1 ms [165.2..166.3] | 5.45 ms |

| Pass | ms |
|---|---|
| `restir.di.initial` | ~145–148 |
| `restir.di.spatial` | 3.6 |
| `restir.di.temporal` | 1.6 |
| `restir.shade` | 0.9 |
| `restir.prepare` | 0.24 |
| `denoise.atrous` | 4.4 |

- Initial sampling does 8 light-tree samples and one visibility ray per pixel, about 30M samples per frame. It runs two orders of magnitude slower than a real-time budget.
- That points at the light-tree traversal / ray-query kernel. Likely causes are per-candidate tree descents with divergent memory access and `-fp-mode precise`.
- This is the top item for WP-7.1 / 7.2. The reuse passes themselves are cheap.

**Gates.** All pass on two runs, and the quality numbers were bit-identical across the runs.

| Gate | Threshold |
|---|---|
| Unbiased ReSTIR vs RIS | relMSE at least 5× lower |
| Biased ReSTIR vs RIS | relMSE at least 2× lower |
| SVGF vs raw ReSTIR | denoised relMSE below raw |
| Bias, unbiased mode and RIS-only | \|bias\| ≤ 2 % |
| Bias, biased mode | \|bias\| ≤ 10 % |
| SVGF frame-to-frame change | ≤ 2 % |
| All values | finite |

No timing gate is applied.

## 7. P9: neural texture compression and gaussian splats vs the conventional path (`fuse_rp_hw_p9_conventional`)

**In-tree pieces used.**
- **NTC.** WP-9.1:
  - `NeuralNet`: a 2D multiresolution hash grid with 16 levels × 2 features, 64×2 hidden layers, fp16 parameters and 6 outputs;
  - the CPU `NeuralTrainer`: Adam, MSE;
  - `NeuralGpu` `neural.infer`, with both the portable and the cooperative-matrix kernel. The 3090 exposes VK_KHR_cooperative_matrix.
- **BCn baseline.** The cook encoders (`Tools/FUSE/Cook`, compiled into the test): BC7 / BC1 for albedo, BC5 for normals, BC4 for roughness.
- **Splats.** WP-9.2 `GsplatRenderer`. The mesh baseline goes through `GpuScene` → `FrameComposer` (T0, mandatory stages only).

### Textures

Per material: 1024² texels, 6 channels (albedo RGB, normal XY, roughness), 3 procedural materials. Quality is the mean over the materials.

| Technique | Bytes (level 0 / with mips) | PSNR albedo / normal / rough (dB) | SSIM | Normal error | Encode | 1440p decode, portable | 1440p decode, coopmat |
|---|---|---|---|---|---|---|---|
| RGBA8 + RG8 + R8 | 7.00 / 9.33 MiB | lossless | 1.0 | 0° | – | texture unit | – |
| BC7 + BC5 + BC4 | 2.50 / 3.33 MiB | 56.0 / 52.1 / 60.9 | 0.999 | 0.26° | ~1 s | texture unit | – |
| BC1 + BC5 + BC4 | 2.00 / 2.67 MiB | 47.2 / 52.1 / 60.9 | 0.994 | 0.26° | ~1.5 s | texture unit | – |
| NTC, hash 2^13 | 0.51 MiB (no mips) | 44.4 / 33.4 / 39.4 | 0.985 | 2.8° | 206–323 s CPU | 140.6 ms | 20.1 ms |
| NTC, hash 2^15 | 2.01 MiB (no mips) | 48.7 / 38.0 / 44.9 | 0.993 | 1.8° | 264–367 s CPU | 141.4 ms | 20.3 ms |

- Decode means 3.69M samples × 6 channels for one material, at 1440p.
- GPU portable decode equals the CPU reference exactly.
- Coopmat decode is within 0.003 of portable and about 7× faster.

**Verdict: not shipped.**
- **Memory: wins on paper.** 2.0 vs 3.3 MiB, but NTC has no mips, so the comparison is not like for like.
- **Quality: loses.** All-channel 41.7 vs 54.2 dB; normals 38 vs 52 dB, which is below even BC1.
- **Cost: loses.** 20 ms per material per frame, against hardware BC sampling.
- **What's missing.** The in-tree net is a generic Instant-NGP-style network, not an NTC latent-grid encoder. It has no mips and no BC fallback, and it fits only on the CPU.

### Splats vs mesh

A textured unit sphere at 2560×1440, covering 1.26M pixels. Quality is the unlit albedo compared against an analytic ray cast.

| Representation | GPU bytes | PSNR / object PSNR (dB) | GPU ms |
|---|---|---|---|
| 100k splats (SH0) | 22.9 MiB | 32.75 / 30.73 | 1.92 |
| 300k splats | 68.7 MiB | 32.36 / 30.28 | 4.11 |
| 1M splats | 228.9 MiB | 33.59 / 33.64 | 29.0 (27.2–30.4; another run 40.0) |
| Mesh, 65k tris + BC7 | 2.57 MiB | 40.76 / 36.09 | 1.88 (cull + vis + resolve + motion) |
| Mesh, 261k tris + RGBA8 | 10.28 MiB | 41.31 / 36.63 | 1.92 |

**Verdict: not shipped as a mesh replacement.**
- **Memory** is 35× the mesh at 1M splats.
- **Quality** is 30–34 vs 36 dB object PSNR.
- **Cost** ties at 100k splats, at worse quality, and is about 15× worse at 1M.
- **Where splats could still win.** Captured or fuzzy content with no mesh equivalent. This bench does not test that case.

**WP-9.2 performance issues found.**
- **`gsplat.preprocess` reads host memory.** It takes 20–31 ms at 1M splats because it reads a 229 MiB host-visible (CpuToGpu) splat buffer over PCIe.
- **`gsplat.scan` runs as a single workgroup.** It takes 5.5 ms.

**Gates.**
- Every step ran and every value is finite.
- Memory accounting is exact.
- GPU decode equals the CPU decode within 1e-4, and coopmat is within 0.1 of portable.
- Sane quality floors.
- The sort dropped no entries.

## 8. Reference scenes (§8): Sponza and Bistro

**Not downloaded.** Downloading needs the user's permission, and the user was away. In place:

| File | What it does |
|---|---|
| `tests/perf/reference_scenes.manifest` | One line per scene: name, URL, archive, **sha256 pin**, marker file, licence. Sources: Morgan McGuire's Computer Graphics Archive — Crytek Sponza (CC BY 3.0) and Amazon Lumberyard Bistro (CC BY 4.0) |
| `cmake/rp_hw_fetch_reference_scenes.cmake` | The fetch step. It downloads only when someone runs it (no ctest does), checks the sha256 and extracts |
| `fuse_rp_hw_bench_refscenes` (ctests `fuse_rp_hw_refscene_{sponza,bistro}`) | The test-time check. It never downloads. It **skips (77)** while an archive is absent or unpinned, **fails** on a sha256 mismatch or a missing marker, and otherwise prints the OBJ statistics |

**The pins are empty.** A sha256 cannot be pinned honestly without seeing the bytes, so this run did not invent values. An empty pin is never trusted: the test skips, and the script refuses to extract unless you opt in to trust on first use.

**How to fetch**, once you have approved the download (about 100 MB for Sponza and about 1 GB for Bistro):

```
cmake -DDEST=<repo>/Samples/reference_scenes -DSCENE=sponza -P Source/FUSE/Renderer/cmake/rp_hw_fetch_reference_scenes.cmake
#   -> prints "UNPINNED archive sha256 = <hash>" and stops. Review the archive, then:
FUSE_REFSCENE_PIN_ON_FIRST_FETCH=1 cmake -DDEST=... -DSCENE=sponza -P ...   # extracts, prints the manifest line
#   commit the printed sha256 into tests/perf/reference_scenes.manifest; later fetches verify it (EXPECTED_HASH)
```

**Asset location and markers.**
- The asset root is the `FUSE_RP_HW_ASSET_ROOT` cache path (default `Samples/reference_scenes`), or the `FUSE_REFSCENE_ROOT` environment variable.
- The marker files (`sponza.obj`, `Exterior/exterior.obj`) are the archives' documented layout. Check them on the first fetch.

**Until then: procedural stand-ins** (`test_rp_hw_bench_scene.hpp`), deterministic:

| Stand-in | Real scene | What the stand-in has |
|---|---|---|
| Sponza-like atrium | 262k tris, 25 materials | 0.55M tris, 244 instances, 24 materials, sun + 8 point lights, transparent banners. Errs heavy |
| Bistro-exterior-like street | ~2.8M tris | 3.92M tris, 2,748 instances, 40 materials, 256 (or 4,096) string lights + sun, glass store fronts |

**Follow-up.** Rendering the real assets in the 1440p harness needs an OBJ → meshlet / GpuScene import step. The E06 cooked-asset path could take a cooked version.

## 9. ctests

| Test | Labels | Result on the RTX 3090 |
|---|---|---|
| `fuse_rp_hw_budget_1440p_sponza` | perf;hw | pass (against the committed baseline) |
| `fuse_rp_hw_budget_1440p_bistro` | perf;hw | pass |
| `fuse_rp_hw_p2_lights_4096` | perf;hw | **fails**: a real miss, 8.97 ms against 3.0 ms |
| `fuse_rp_hw_p3_vsm_cached` | perf;hw | pass |
| `fuse_rp_hw_p5_dag_100m` | perf;hw | pass (1.85 ms; three consecutive runs after the idle-clock fix) |
| `fuse_rp_hw_p7_restir_10k` | perf;hw | pass (quality gates; no timing gate) |
| `fuse_rp_hw_p8_volumetrics` | perf;hw | pass |
| `fuse_rp_hw_p9_conventional` | perf;hw | pass (robust-fact gates; the verdicts are in §7) |
| `fuse_rp_hw_refscene_{sponza,bistro}` | perf;hw | skip (77): assets absent |

Every test is RUN_SERIAL, exits 77 without a discrete GPU (Lavapipe CI, stub trees), and has no `ENVIRONMENT`, so it sees the real ICDs.

**Run them:**

```
ctest -L hw --output-on-failure
```

**Re-baseline after an intended change:**

```
FUSE_UPDATE_PERF_BASELINE=1 ctest -R fuse_rp_hw_budget
```
