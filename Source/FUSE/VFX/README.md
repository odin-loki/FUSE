# fuse_vfx — B7.7 VFX System

CPU-first particle VFX scaffolding for Track B7.7. Implements emitter descriptors, SoA particle storage with free-list slot recycling, jobified CPU reference simulation, effect instances, and a `ParticleSystem` facade. GPU buffer layout stubs (`particle_gpu.hpp`), CUDA dispatch counts, and a CPU mirror pack/unpack path support stub tests ahead of device kernels. Full CUDA simulation and billboard rendering remain deferred.

## Layout

| Header | Role |
|--------|------|
| `vfx_desc.hpp` | System limits and backend selection |
| `particle_emitter.hpp` | `ParticleEmitterDesc`, CPU `ParticleSoA`, `ParticleEmitter` |
| `particle_soa_ops.hpp` | Standalone `burst_emit`, `accumulate_rate_emit`, `lifetime_cull`, `simulate_step` CPU stubs |
| `particle_gpu.hpp` | `ParticleGpuBufferLayout`, `ParticleGpuDispatch`, `ParticleGpuMirror`, `ParticleSoAGPU` |
| `effect_instance.hpp` | Spawned effect playback state |
| `particle_system.hpp` | Emitter/effect registry and frame update |

## P7 pipeline (stub)

`spawn → simulate → (render deferred)`

- **Emit** — rate-based or burst emission into CPU SoA slots via O(1) free-list allocation (`free_slot_count()` for diagnostics)
- **Simulate** — gravity, drag, lifetime aging, size/color/alpha interpolation over `parallel_for` (serial when the job scheduler is single-threaded); non-positive `dt` and disabled emitters are no-ops
- **Render** — not implemented in this milestone

## Particle SoA

`ParticleSoA` stores per-particle columns (`positions`, `velocities`, `ages`, `lifetimes`, `sizes`, `colors`, `alphas`, `alive_flags`) dense over a fixed capacity. Dead slot indices live in `free_slots` so burst and rate emission avoid linear scans. Simulation integrates each live slot independently; expired slots return to the free list after the parallel pass.

## Tests

`fuse_vfx_tests` (`ctest` name `fuse_vfx_runtime`) covers standalone SoA burst/rate/cull/sim helpers (emit count reporting, `burst_emit(0)`, deterministic seed fill, `lifetime_cull` partial/full expiry without integration, age/kill via `simulate_step`, parallel parity), emitter burst/rate emission (including `burst(0)`, capacity clamp, burst+rate interleave), free-list slot recycling after partial expiry, attribute interpolation, drag integration, parallel vs single-thread simulation parity (grain boundaries, single particle, all-dead, multi-worker), GPU buffer layout alignment (per-column 16-byte offsets, padding, multi-capacity validation), simulate/emit dispatch counts (`forFrame`, block boundaries, `gridDimX`), CPU mirror pack/unpack round trips (full-capacity fill, undersized unpack guard), `spawn_effect` burst_count, effect instance duration, system spawn/update cleanup, and handle lifecycle without GPU or renderer dependencies.

## Build

Built with `FUSE_BUILD_CORE=ON`. Tests run when `FUSE_BUILD_CORE_TESTS=ON`.
