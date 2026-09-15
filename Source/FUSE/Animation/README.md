# fuse_animation — B7.1 Animation System

CPU-first skeletal animation scaffolding for Track B7.1. Implements the P7 pipeline stages:

`sample → blend → solve IK → skin`

## Layout

| Header | Role |
|--------|------|
| `skeleton.hpp` | Bone hierarchy, bind pose, `Pose` (AoS), `PoseSoA` (local TRS columns) |
| `clip.hpp` | Keyframe channels, `evaluate` (SoA) and `sample` (AoS) |
| `blend_tree.hpp` | `ClipNode`, `BlendNode2`, `BlendSpace1D/2D`, `LayeredBlendNode`, state machine |
| `ik_solver.hpp` | FABRIK and two-bone IK stubs |
| `skinning.hpp` | CPU linear blend skinning (`Cuda` when `FUSE_HAS_CUDA`) |
| `animator.hpp` | Game-thread animator component facade |

## Clip evaluation

`AnimationClip::evaluate` samples sparse per-bone position/rotation/scale channels into a `PoseSoA`, then walks the skeleton hierarchy to produce world-space bone matrices. `sample` wraps `evaluate` and returns the legacy `Pose` AoS layout for skinning callers.

## Blend tree (stub)

- **BlendSpace1D** — bracketing blend between clips along one runtime parameter (e.g. speed).
- **BlendSpace2D** — inverse-distance weighted blend in a 2D parameter plane (velocity X/Y stub).
- **LayeredBlendNode** — masked upper-body layer over a base pose using `PoseSoA` local TRS blending.

## Tests

`fuse_animation_tests` (`ctest` name `fuse_animation_runtime`) covers skeleton hierarchy, `PoseSoA` propagation and buffer clear/reuse, `blend_pose_soa` output reuse, clip evaluate/sample, blend interpolation, 1D blend space, layered mask weight sweeps (0/1/fractional/clamped) and unmasked-bone isolation, state transitions, IK convergence, skinning, and animator ticks without GPU or editor dependencies.

Track B narrative: [docs/unification/TRACK-B-ANIMATION.md](../../../docs/unification/TRACK-B-ANIMATION.md).
