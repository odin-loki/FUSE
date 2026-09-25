# Renderer golden images (WP-0.7)

Reference frames for the renderer golden gates (`ctest -L golden`), produced by the WP-0.7 harness
in `Source/FUSE/Renderer/tests/harness/` and registered by `Source/FUSE/Renderer/cmake/rp_harness.cmake`.

| File | Scene (builder in `harness/scene.cpp`) | Test(s) |
|---|---|---|
| `gbuffer_quads.png` | two screen-space quads through the **stock** `gbuffer.vert/frag` | `rp_golden_gbuffer_roundtrip` (exit test) |
| `cornell_box.png` | Cornell box, emissive ceiling light, two rotated blocks | `rp_golden_gbuffer_roundtrip`, `rp_golden_cornell_box`, `rp_golden_cornell_box_t0/t1/t2` |
| `sphere_field.png` | 4x4 faceted spheres, roughness / metallic ramps | `rp_golden_sphere_field` |
| `thin_wall.png` | two rooms split by a 0.02-unit wall, one lit by an emissive panel (GI leak scene) | `rp_golden_thin_wall` |
| `hud_overlay.png` | Cornell box plus the screen-space HUD layer | `rp_golden_hud_overlay` |
| `instance_grid_1k.png`, `instance_grid_100k.png` | 1k / 100k cubes, same draw count | `rp_golden_instance_grid_1k`, `rp_golden_instance_grid_100k` |

Each image is the CPU resolve (fixed half-Lambert light + emissive, sRGB) of the six G-buffer
attachments that `GBufferRasterPass` wrote on Lavapipe, so a change anywhere in the raster path, the
`write_gbuffer()` packing, the scene builders or the readback shows up here.

## Gate

A check passes when **both** hold (`harness/golden.hpp`):

1. **Metric**: mean FLIP <= 0.01 through `fuse/renderer/quality/image_metrics.hpp` (SSIM is also
   available). When the image-metrics library is not linked, PSNR >= 40 dB is used instead.
2. **Pixel budget**: at most `maxDifferingPixels` (default **0**) pixels with any channel differing by
   more than `pixelTolerance` (default 2 of 255). Lavapipe is deterministic, so a single changed pixel
   fails even though the mean metric barely moves (the exit test checks this on every run).

On failure the test writes `<build>/rp_harness_artifacts/<name>.actual.png`, `<name>.diff.png`
(golden dimmed to grey, failing pixels red) and `<name>.gbuffer.exr` (albedo, depth, emissive and
normal channels) and prints their paths.

## Updating

```sh
FUSE_UPDATE_GOLDENS=1 ctest --test-dir build/fuse-debug -L golden
```

Update mode rewrites only goldens that changed and ends with a summary listing every rewritten file
and its size. Review the new images (and the diff of the old ones) before committing, and say in the
commit message why the output changed. Never regenerate to silence a failure you cannot explain.

## Storage policy

* PNG only (8-bit sRGB, RGB when opaque), written by the harness's own encoder, so the bytes are
  reproducible. EXR is for failure artefacts in the build tree, never for goldens.
* 128x128 by default; keep each file under **64 KiB** (the update summary warns above that) and the
  whole directory under 1 MiB. Current total is about 53 KB; the 100k-instance grid is the largest
  (~30 KiB, high-frequency content).
* Goldens are committed in-tree (no Git LFS): they are small, and CI must run them without extra
  fetches. Large reference scenes (Sponza, Bistro) are downloaded at test time, not stored here.
* Goldens are per renderer path, not per tier: the `rp_golden_cornell_box_t<N>` tests assert the
  legacy raster path renders the same golden on devices capped at T0, T1 and T2. A path that is
  legitimately tier-specific uses `--golden-name-suffix _t<N>` and its own file.
* Goldens come from Lavapipe (Mesa llvmpipe). A hardware runner that cannot match bit-for-bit gets a
  per-scene tolerance or pixel budget in `specFor()` (`test_rp_harness_golden.cpp`), never a global
  relaxation.
