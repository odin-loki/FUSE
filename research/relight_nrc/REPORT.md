# Relight RL-5.4: hash-grid radiance cache vs an online-trained neural radiance cache

Research track of plan package RL-5.4 (`docs/plans/FUSE_REMIX_PORT_PLAN.md` §5.4, "Neural cache (research, T3 HW)").
The plan asks for reports only. The neural cache ships only if it beats the hash grid on the RTX 3090 (renderer plan,
Phase 9 rule). This report compares the two caches on the CPU reference path tracer. The hardware comparison is still
open (see the last section).

## What is compared

Both caches are driven by the same machinery (`Source/FUSE/Relight/render/pathtrace/radiance_cache*`,
`Source/FUSE/Relight/kernels/radiance_cache_*`):

- **Training data.** Each frame traces one training path per 2 x 2 pixel tile, using the path tracer's own core with
  `kPtFlagRcTrain`. Every scattering vertex k becomes a record: its position, its facing normal, and the one-sample
  estimate `L_k = (R - R_k) / thr_k` of the radiance it scatters back.
- **Termination.** A path ends at the first vertex after the G-buffer vertex that meets three conditions: it is opaque,
  its perceptual roughness is at least 0.3, and its path spread (Bekaert et al. 2003; Müller et al. 2021) has reached
  the vertex's adaptive cell size.
- **Answer.** This is the only part that differs between the two caches:
  - **Hash grid** (the default). The resolved radiance of the vertex's cell. Cells are spatially hashed after Binder
    et al. 2019, with the per-distance level of detail of Gautron 2020 and six normal bins. Samples are accumulated with
    integer atomics, blended over an exponential temporal window of 512 samples, and evicted after 16 idle frames.
  - **Neural** (`research/relight_nrc`). This is a small MLP after Müller et al. 2021, "Real-time Neural Radiance
    Caching for Path Tracing", built on the renderer's WP-9.1 runtime (`fuse_neural`). It has the following parts:
    - an Instant-NGP multiresolution hash-grid encoding (12 levels x 2 features, 2^14 entries per level, base
      resolution 4, scale 1.5);
    - two hidden layers of 32 ReLU units and 3 linear outputs, clamped at 0 when queried;
    - MSE on the clamped one-sample targets, with Adam at a learning rate of 1e-2 and 4 steps per frame over that
      frame's records (about 700 records per frame);
    - the input is the position, offset by 0.02 along the normal and normalised to the scene bounds (the WP-9.1 hash
      grid encodes at most 3 raw inputs).

    Training runs on the CPU (`NeuralTrainer`). Inference runs on the CPU, and on the GPU through WP-9.1 `NeuralGpu`.

Both caches get an equal training budget: the same training paths and the same termination decisions. The comparison
therefore isolates the function approximator.

## Results (Cornell box, 32 x 32, max 6 bounces, Russian roulette from bounce 3)

Measurement setup:

- **Bias.** 16 warm-up frames, then 64 frames of cache update plus 16 spp of cache-terminated rendering. The result is
  compared with the RL-5.1 reference (plain path tracing at 1024 spp).
- **Variance.** Rendered at pixel centres from a cache frozen after 32 frames. The per-pixel variance at 64 spp is summed
  over the image and divided by the same sum for plain path tracing.
- **Source.** `rl_nrc_report` writes `nrc_report.json`. The numbers below come from the MinGW build under Wine. The
  Linux build agrees to within about 1e-3; the neural row differs slightly because of libm.

| cache | image bias r / g / b | 4x4 block error rms / max | variance vs plain PT | hit rate | CPU ms / frame (cache work) |
|---|---|---|---|---|---|
| hash grid | -0.57% / -0.16% / +0.04% | 2.4% / 9.1% | 0.52 | 0.988 | 3.9 |
| neural (MLP) | -0.09% / -0.21% / +0.75% | 1.9% / 6.8% | 0.49 | 1.000 (always answers) | 34.3 |

The reference's own noise is about 0.17% of the image mean. At 4x4 block level, the noise of the reference plus the
cache run is about 2%, so the block errors are mostly noise, not bias.

Findings:

- **Bias.** Both caches stay well inside the hash grid's gate. The hash grid is slightly darker in red: the red wall's
  bounce light is averaged over cells that reach across the corner. The MLP's continuous fit avoids some of this cell
  quantisation, but it adds a small, colour-dependent fitting error (+0.75% blue).
- **Variance.** Ending paths in either cache roughly halves the per-pixel variance at equal spp. The MLP is slightly
  smoother: it interpolates, whereas the grid is piecewise constant and keeps some of its per-cell sample noise.
- **Cost.** On the CPU, one frame's cache work costs about 10 times more for the neural cache (training dominates).
  This says nothing about the GPU: the WP-9.1 portable kernel infers the trained net at the training records bit for bit
  (`rl_pe.nrc_vk`: 358 queries per frame, worst relative error 0, zero validation messages). On-GPU training does not
  exist yet.
- **Convergence of the fit.** Measured at frame 4's record positions, the network's RMS error against the converged hash
  grid falls from 0.077 after 4 frames to 0.043 after 64 frames (`rl_nrc_train`). The one-sample batch loss stays
  around 0.02, which is mostly the variance of the targets.

## Hash-grid gates (the default cache)

The documented bias bound is enforced by `rl_radiance_cache_converge`: cache-terminated paths must match the RL-5.1
reference within 2% of the image mean per channel, and within 6% RMS over 4x4 blocks.

| scene | image bias r / g / b | block rms / max | hit rate |
|---|---|---|---|
| Cornell | 0.57% / 0.16% / 0.04% | 2.4% / 9.1% | 0.988 |
| Cornell + glass sphere | 0.66% / 0.65% / 0.23% | 2.8% / 9.7% | 0.989 |

The GPU gate (`rl_pe.radiance_cache_vk`, Lavapipe under Wine, Slang and GLSL) checks four things:

- **Parity.** The update, resolve and query stages match the CPU replay exactly, per key, on the GPU's read-back inputs.
  This covers the cell ids, the integer sums, the resolved radiance, and the lookup at every training record. The
  training records and the cache-terminated trace also match the CPU for all 576 pixels.
- **Bias.** 64 GPU frames with the cache stay within 1.2% of the CPU reference.
- **Determinism.** Two runs are bit-identical.
- **Allocations and validation.** Steady-state frames make zero allocations, and there are zero validation messages
  under sync validation.

## Limitations and open issues

- **Hardware comparison.** Neither cache has been measured on the RTX 3090. The plan's decision rule (the neural cache
  ships only if it beats the hash grid there) cannot be applied yet. The neural cache would first need these parts:
  - on-GPU training (a backward pass and Adam in WP-9.1);
  - fused inference inside the path tracer's trace shader (today the GPU trace pass only queries the hash grid);
  - cooperative-matrix kernels (the WP-9.1 coopmat inference kernel exists but is not used here).
- **Scale.** The CPU results are small-scale: 32 x 32 pixels, one scene, and diffuse-dominated. At this size the neural
  cache's advantage is within noise. Glossy view-dependent effects are out of reach for both caches as configured: the
  hash grid has no direction bins, and the MLP has no direction input.
- **Neural inputs.** The neural cache uses only the position (offset along the normal) as input. Müller et al. also feed
  the direction, normal, roughness and albedo, and use a relative L2 loss. The WP-9.1 encoding limits the hash grid to 3
  raw inputs; adding a second (frequency / one-blob) encoding for the other inputs would need WP-9.1 changes.
- **Licensing.** The code is written from the cited papers only. No NVIDIA NRC SDK, SHaRC or tiny-cuda-nn code or
  binaries are used.
