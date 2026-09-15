# fuse_vfx — B7.7 VFX System (stub)

CPU-first particle VFX scaffolding for Track B7.7. Implements emitter descriptors, CPU reference simulation, effect instances, and a `ParticleSystem` facade. GPU/CUDA simulation and billboard rendering are deferred.

## Layout

| Header | Role |
|--------|------|
| `vfx_desc.hpp` | System limits and backend selection |
| `particle_emitter.hpp` | `ParticleEmitterDesc`, CPU `ParticleSoA`, `ParticleEmitter` |
| `effect_instance.hpp` | Spawned effect playback state |
| `particle_system.hpp` | Emitter/effect registry and frame update |

## P7 pipeline (stub)

`spawn → simulate → (render deferred)`

- **Emit** — rate-based or burst emission into CPU SoA slots
- **Simulate** — gravity, drag, lifetime aging (SDF collision stubbed off)
- **Render** — not implemented in this milestone

## Tests

`fuse_vfx_tests` (`ctest` name `fuse_vfx_runtime`) covers emitter burst/rate simulation, effect instance duration, system spawn/update cleanup, and handle lifecycle without GPU or renderer dependencies.

## Build

Built with `FUSE_BUILD_CORE=ON`. Tests run when `FUSE_BUILD_CORE_TESTS=ON`.
