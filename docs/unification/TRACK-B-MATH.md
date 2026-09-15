# Track B — Core Math Library (B1.4)

**Status:** B1.4 deepen — Mat3/Mat4, Quat, AABB, Frustum basics landed in `fuse::math`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B1.4  
**Layout:** [unified-layout.md](./unified-layout.md) — `Source/FUSE/Core/include/fuse/math/`

---

## Scope

| Type / module | Header | Notes |
|---------------|--------|-------|
| `Vec2`, `Vec3`, `Vec4` | `vec.hpp` | POD vectors; `cross`, `dot`, `normalized` |
| `Mat3`, `Mat4` | `mat.hpp` | Column-major; `fromTRS`, `perspective`, `lookAt`, `inverseAffine` |
| `Quat` | `quat.hpp` | `(x,y,z,w)` with `w` scalar; `rotate`, `slerp`, `fromAxisAngle` |
| `AABB` | `aabb.hpp` | Overlap, merge, contains, slab ray intersection |
| `Frustum` | `frustum.hpp` | Extract from view-projection; sphere/AABB intersection tests |
| SDF primitives | `sdf.hpp` | `sphere`, `box`, `opSmoothUnion` (existing) |
| Umbrella include | `math.hpp` | Pulls all public math headers |

**Not in scope (deferred):** SIMD `vec4` lanes, CUDA `FUSE_HOST_DEVICE` noise library, Torque `Point3F`/`MatrixF` compat shims, migration of `fuse::ecs::vec3` / `fuse::spatial` aliases.

---

## Conventions

- **Matrices:** column-major storage (`data[row + col * stride]`), OpenGL/Vulkan layout.
- **Quaternions:** Hamilton product; engine internals use quats — Euler angles stay at editor input only.
- **Frustum planes:** six normalized `Vec4` coefficients `(nx, ny, nz, d)` from the combined view-projection matrix.
- **Tests:** `fuse_core_math_tests` in `Source/FUSE/Core/tests/test_math.cpp`.

---

## Downstream consumers

Renderer (`fuse::math::Vec3` in clustered lighting, TAA, atmosphere), Compute (SDF ray march), and future ECS/Spatial unification should prefer `fuse::math` over duplicate `fuse::ecs::vec3` PODs as B1.4 gates close.
