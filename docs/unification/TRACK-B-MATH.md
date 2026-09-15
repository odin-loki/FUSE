# Track B — Core Math Library (B1.4)

**Status:** B1.4 SIMD deepen — `fuse::math::simd` CPU stubs (Mat4/AABB), plane classify/clip helpers, edge-case tests  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B1.4  
**Layout:** [unified-layout.md](./unified-layout.md) — `Source/FUSE/Core/include/fuse/math/`

---

## Scope

| Type / module | Header | Notes |
|---------------|--------|-------|
| `Vec2`, `Vec3`, `Vec4` | `vec.hpp` | POD vectors; `cross`, `dot`, `normalized` |
| `Mat3`, `Mat4` | `mat.hpp` | Column-major; `fromTRS`, `perspective`, `lookAt`, `inverseAffine` |
| `Quat` | `quat.hpp` | `(x,y,z,w)` with `w` scalar; `rotate`, `slerp`, `fromAxisAngle` |
| `AABB` | `aabb.hpp` | Overlap, merge, contains, slab ray intersection, `transformAabb`, `transformAabbCorners` |
| `Plane` | `plane.hpp` | `classifyPoint`, `classifyAabb`, `clipSegmentAgainstPlane`, `clipPolygonAgainstPlane` |
| `simd` | `simd.hpp` | CPU stubs: `Float4`/`Mat4` lane layout, multiply/inverse/orthonormalize, AABB transform/merge/ray |
| `Frustum` | `frustum.hpp` | Extract from view-projection; sphere/AABB intersection tests |
| SDF primitives | `sdf.hpp` | `sphere`, `box`, `opSmoothUnion` (existing) |
| Umbrella include | `math.hpp` | Pulls all public math headers |

**Not in scope (deferred):** intrinsics-backed `__m128` lanes, CUDA `FUSE_HOST_DEVICE` noise library, Torque `Point3F`/`MatrixF` compat shims, migration of `fuse::ecs::vec3` / `fuse::spatial` aliases, full 4×4 inverse for non-uniform scale.

---

## Conventions

- **Matrices:** column-major storage (`data[row + col * stride]`), OpenGL/Vulkan layout.
- **Quaternions:** Hamilton product; engine internals use quats — Euler angles stay at editor input only.
- **Frustum planes:** six normalized `Vec4` coefficients `(nx, ny, nz, d)` from the combined view-projection matrix.
- **Tests:** `fuse_core_math_tests` in `Source/FUSE/Core/tests/test_math.cpp`.
- **AABB transforms:** `transformAabb` uses the absolute linear-part envelope (fast path for rigid transforms); `transformAabbCorners` transforms all eight corners (exact reference, required for non-uniform scale).
- **Mat4 inverse:** `inverseAffine` requires an orthogonal 3×3 upper block (rotation ± translation). Non-uniform scale needs a full 4×4 inverse (deferred).

---

## B1.4 deepen follow-up coverage

| Area | Tests |
|------|-------|
| Mat4 multiply | Left/right identity, associativity, chained TRS vs staged transforms |
| Mat4 inverse | Identity, pure translation, pure rotation, rigid-body left/right product |
| AABB transform | Translation, rotation envelope vs corner reference, non-uniform scale corners |
| `fuse::math::simd` | Lane-layout Mat4 multiply/inverse parity vs scalar; `orthonormalize` stub; AABB transform/merge/ray stubs |
| Plane helpers | Point/AABB classification, segment clip, convex polygon clip against one plane |

---

## Downstream consumers

Renderer (`fuse::math::Vec3` in clustered lighting, TAA, atmosphere), Compute (SDF ray march), and future ECS/Spatial unification should prefer `fuse::math` over duplicate `fuse::ecs::vec3` PODs as B1.4 gates close.
