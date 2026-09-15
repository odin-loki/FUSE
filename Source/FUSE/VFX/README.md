# fuse_vfx — B7.7 VFX System

CPU-first particle VFX scaffolding for Track B7.7. Implements emitter descriptors, SoA particle storage with free-list slot recycling, jobified CPU reference simulation, effect instances, and a `ParticleSystem` facade. GPU/CUDA simulation and billboard rendering are deferred.

## Layout

| Header | Role |
|--------|------|
| `vfx_desc.hpp` | System limits and backend selection |
| `particle_emitter.hpp` | `ParticleEmitterDesc`, CPU `ParticleSoA`, `ParticleEmitter` |
| `effect_instance.hpp` | Spawned effect playback state |
| `particle_system.hpp` | Emitter/effect registry and frame update |

## P7 pipeline (stub)

`spawn → simulate → (render deferred)`

- **Emit** — rate-based or burst emission into CPU SoA slots via O(1) free-list allocation
- **Simulate** — gravity, drag, lifetime aging, size/color/alpha interpolation over `parallel_for` (serial when the job scheduler is single-threaded)
- **Render** — not implemented in this milestone

## Particle SoA

`ParticleSoA` stores per-particle columns (`positions`, `velocities`, `ages`, `lifetimes`, `sizes`, `colors`, `alphas`, `alive_flags`) dense over a fixed capacity. Dead slot indices live in `free_slots` so burst and rate emission avoid linear scans. Simulation integrates each live slot independently; expired slots return to the free list after the parallel pass.

## Tests

`fuse_vfx_tests` (`ctest` name `fuse_vfx_runtime`) covers burst/rate emission, capacity clamping, slot recycling, attribute interpolation, drag integration, parallel vs single-thread simulation parity, effect instance duration, system spawn/update cleanup, and handle lifecycle without GPU or renderer dependencies.

## Build

Built with `FUSE_BUILD_CORE=ON`. Tests run when `FUSE_BUILD_CORE_TESTS=ON`.
