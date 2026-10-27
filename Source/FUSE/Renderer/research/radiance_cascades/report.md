# WP-6.5 — Radiance cascades (research prototype)

**Status:** Metrics-only research library (`fuse_rc_research`). Flatland 2D cascades are not wired into `FrameComposer` (no per-texel scene buffer on the deferred path). Near-field radiance cascades in production use WP-6.5b **`fuse_ssrc_gpu`** (`FrameSettings::ssrc`, stage `kStageSsrc`, gate `fuse_rp_frame_vk_ssrc_t0`).

## What shipped

Flatland radiance cascades (Sannikov 2023) on a 2D probe grid:

- **Spacing:** each cascade doubles probe spacing; directions and interval length grow per cascade (`branch` 4 default, 2 for penumbra-exact mode).
- **Merge:** vanilla bilinear merge vs **bilinear fix** (reduces disk-emitter over-estimation and sealed-box leak).
- **CPU reference:** `rc_reference.cpp` — line-for-line twin of the Slang/GLSL kernels.
- **Brute force:** per-pixel ray tracing ground truth for metric scenes.
- **Vulkan:** `RadianceCascadesGpu` (`rc.cascade` × n coarse-to-fine, `rc.gather`, `rc.copy`) on render graph v2 with BDA-only `RcPush` (112 bytes).

## CTest gates (`fuse_rp_rc_*`)

| Gate | What it proves |
|------|----------------|
| `fuse_rp_rc_layout` | `RcPush` size and field offsets match GLSL/Slang mirrors; cascade grid layout (2× spacing, 4× directions, contiguous intervals). |
| `fuse_rp_rc_energy` | Empty scene → sky radiance (≤1e-6 error); lone disk emitter mean within 10% of brute force (bilinear fix; vanilla is looser). |
| `fuse_rp_rc_error` | Rooms scene vs brute-force L1: bilinear fix ≤ 0.12, vanilla ≤ 0.18; fix beats vanilla. |
| `fuse_rp_rc_leak` | Sealed box beside bright emitter: fix leak ≤ 1e-6 of outside mean; vanilla leak measured and must exceed fix. |
| `fuse_rp_rc_ringing` | Disk emitter angular non-uniformity: fix ≤ brute + 0.02 and ≤ vanilla. |
| `fuse_rp_rc_cost` | Samples/pixel and records/cascade scale for 32²…256²; branching-4 keeps records/cascade constant. |

GPU counterparts live in `tests/test_rp_rc_gpu.cpp` (Lavapipe / validation-layer gated).

## Recommendation (unchanged)

Use screen-space radiance cascades (`fuse_ssrc_gpu`) as a **near-field supplement** to DDGI, not a replacement. Full 3D cascades in the frame remain a Track B follow-up after WP-6.5b metrics are stable on CI.

## References

- Engineering log: [`docs/unification/RENDERER-EXECUTION.md`](../../../../docs/unification/RENDERER-EXECUTION.md) (WP-6.5 row).
- Source: `Source/FUSE/Renderer/research/radiance_cascades/`.
