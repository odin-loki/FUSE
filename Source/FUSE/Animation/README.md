# fuse_animation — B7.1 Animation System (stub)

CPU-first skeletal animation scaffolding for Track B7.1. Implements the P7 pipeline stages as stubs:

`sample → blend → solve IK → skin`

## Layout

| Header | Role |
|--------|------|
| `skeleton.hpp` | Bone hierarchy, bind pose, world transforms |
| `clip.hpp` | Keyframe channels and clip sampling |
| `blend_tree.hpp` | Clip nodes, 2-way blend, state machine transitions |
| `ik_solver.hpp` | FABRIK and two-bone IK stubs |
| `skinning.hpp` | CPU linear blend skinning (`Cuda` when `FUSE_HAS_CUDA`) |
| `animator.hpp` | Game-thread animator component facade |

## Tests

`fuse_animation_tests` (`ctest` name `fuse_animation_runtime`) covers skeleton hierarchy, clip sampling, blend interpolation, state transitions, IK convergence, skinning, and animator ticks without GPU or editor dependencies.
