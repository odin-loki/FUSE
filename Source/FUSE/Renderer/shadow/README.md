# B5.5 Shadow System

Cascaded shadow maps for the directional light plus SDF soft shadows (P5 §5.5):

`CSM descriptor → shadow atlas → DirectionalShadow update → ShadowPass render graph node`

- **CSM** — 4 cascades with uniform/log/practical splits. With `CascadedShadowMapDesc::stabilise`
  (default on) each cascade is fitted to the bounding sphere of its frustum slice and snapped to a
  world-anchored texel grid, so the shadow map does not shimmer when the camera moves or rotates.
  The light basis handles a vertical sun without NaNs.
- **SDF soft shadows** — `sdfSoftShadow` (`sdf_soft_shadow.hpp`) is the CPU reference of the CUDA
  helper: improved-distance penumbra march that also resolves the umbra side, with a disc-visibility
  falloff where `penumbraK = 1 / light angular radius`. The CUDA kernel is not written yet.
- **GPU side** — `ShadowAtlas` / `DirectionalShadow` allocate R32F colour targets per cascade and
  `ShadowPass` records the `shadow_maps` render-graph pass.

## Layout

| Header | Role |
|--------|------|
| `shadow/csm.hpp` | `CascadedShadowMapDesc` / `Data`, split schemes and helpers, `CascadeLightSpaceLayout` (light view, slice bounding sphere, stabilised ortho bounds, light-space AABB fit) |
| `shadow/sdf_soft_shadow.hpp` | `sdfSoftShadow`, `sdfSoftShadowClearance`, `sdfDiscVisibility` |
| `shadow/shadow_atlas.hpp` | `ShadowAtlas` — packs cascades into a single atlas |
| `shadow/directional_shadow.hpp` | `DirectionalShadow` — owns atlas + per-cascade targets, updates cascade matrices |
| `shadow/shadow_pass.hpp` | `ShadowPass` — inserts the `shadow_maps` render-graph pass |

## Tests

- `fuse_b5_shadows_gates` — B5.5 gate rows: cascades agree with a reference at every boundary (0
  mismatches, no seams); stabilised frame diff 0 px vs 24 px unstabilised; SDF penumbra width linear
  in occluder distance; SDF shadow within 2.97% of a 1600-spp path tracer (0 px over 5%).
- `fuse_csm_guards` — sanitize and skip guards for degenerate cascade inputs.
- `fuse_shadow_system` — split schemes, count clamp, batch helpers, light-space AABB containment,
  atlas layout, allocation/update, render-graph pass registration.

GPU timings (SDF shadows < 2 ms) are hardware-only.

## Upstream / downstream

- **Depends on:** `fuse_rhi` resource manager (B2.3), deferred frame pipeline slot `ShadowMaps` (B5.1)
- **Future:** CUDA SDF soft-shadow kernel in `fuse_compute`, deferred shading sampling (B5.4)
