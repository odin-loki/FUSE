# B5.5 Shadow System (stub)

CPU-side cascaded shadow map scaffolding for Track B5.5. Implements the P5 §5.5 directional shadow pipeline as allocation and layout stubs:

`CSM descriptor → shadow atlas → DirectionalShadow update → ShadowPass render graph node`

## Layout

| Header | Role |
|--------|------|
| `shadow/csm.hpp` | `CascadedShadowMapDesc`, `CascadedShadowMapData`, uniform/log/practical split helpers, cascade count clamp, split-distance/near-distance batch helpers, non-empty cascade count, empty-frustum detection, `CascadeLightSpaceLayout` world-corner AABB fit/containment + variable-count batch matrix stubs |
| `shadow/shadow_atlas.hpp` | `ShadowAtlas` — packs cascades into a single depth atlas |
| `shadow/directional_shadow.hpp` | `DirectionalShadow` — owns atlas + per-cascade depth targets |
| `shadow/shadow_pass.hpp` | `ShadowPass` — inserts the `shadow_maps` render-graph pass |

## Tests

`fuse_shadow_system` covers cascade split schemes (uniform/log/practical), count clamp (including single-cascade), split-distance/near-distance batch validation, non-empty cascade counting, light-space AABB corner containment + ortho containment + empty frustum, variable-count batch matrix builder, 2×2 atlas layout, directional shadow allocation/update, and render-graph pass registration.

## Upstream / downstream

- **Depends on:** `fuse_rhi` resource manager (B2.3), deferred frame pipeline slot `ShadowMaps` (B5.1)
- **Future:** SDF soft shadows in `fuse_compute` (B5.5 CUDA kernel), deferred shading sampling (B5.4)
