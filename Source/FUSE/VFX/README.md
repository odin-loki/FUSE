# fuse_vfx — B7.7 VFX System

CPU particle VFX for Track B7.7: emitter descriptors, SoA particle storage with free-list slot recycling, a jobified CPU simulation (exact rate emission, SDF plane/sphere collision, deterministic for any worker count), effect instances, and a `ParticleSystem` facade. The B7.7 CPU rows are proven by `fuse_b7_vfx_gates`. `particle_gpu.hpp` holds the GPU buffer layout, dispatch counts and a CPU mirror pack/unpack path; the simulation step is the single-source `particle_sim_kernel.hpp` (item update + count/scan/write compaction; CPU backends everywhere, `kernels/particle_sim.cu` when `FUSE_HAS_CUDA` and a device exists, gate `fuse_particle_kernel_parity`); billboard rendering is not written yet.

## Layout

| Header | Role |
|--------|------|
| `vfx_desc.hpp` | System limits and backend selection |
| `particle_emitter.hpp` | `ParticleEmitterDesc`, CPU `ParticleSoA`, `ParticleEmitter` |
| `particle_soa_ops.hpp` | Standalone `burst_emit`, `accumulate_rate_emit`, `clamp_burst_count` / `is_at_capacity` / `has_live_particles`, `free_slot_count` / `sync_alive_count`, `recycle_slots`, `lifetime_cull`, `simulate_step` CPU helpers |
| `particle_gpu.hpp` | `ParticleGpuBufferLayout`, `ParticleGpuDispatch`, `ParticleGpuMirror`, `ParticleSoAGPU` |
| `effect_instance.hpp` | Spawned effect playback state |
| `particle_system.hpp` | Emitter/effect registry and frame update |

## P7 pipeline

`spawn → simulate → (render deferred)`

- **Emit** — rate-based or burst emission into CPU SoA slots via O(1) free-list allocation (`free_slot_count()` for diagnostics)
- **Simulate** — gravity, drag, lifetime aging, size/color/alpha interpolation through `kernel::launch` (`particle_update` + `particle_compact`; CpuReference below 16384 slots, CpuParallel above, bit-identical); non-positive `dt` and disabled emitters are no-ops
- **Collide** — when `collide_with_world` is set, each live particle is resolved against the analytic SDF colliders in `ParticleEmitterDesc::colliders` (planes and solid spheres): pushed back to the surface, normal velocity reflected with `restitution`, tangential velocity scaled by `1 - friction`
- **Rate emission** — the emitter accumulates `emit_rate * dt` in double precision, so the emitted total is exactly `floor(rate * t)` (`total_emitted()`); expired slots are recycled in sorted order, so results are bit-identical for any worker count
- **Render** — not implemented in this milestone

## Particle SoA

`ParticleSoA` stores per-particle columns (`positions`, `velocities`, `ages`, `lifetimes`, `sizes`, `colors`, `alphas`, `alive_flags`) dense over a fixed capacity. Dead slot indices live in `free_slots` so burst and rate emission avoid linear scans. Simulation integrates each live slot independently; expired slots return to the free list after the parallel pass.

## Tests

`fuse_vfx_tests` (`ctest` name `fuse_vfx_runtime`) covers standalone SoA burst/rate/cull/sim helpers (requested/emitted/clamped counts, `clamp_burst_count` / `is_at_capacity` / `has_live_particles`, `burst_emit(0)` and zero-capacity guards, `free_slot_count` / `sync_alive_count`, deterministic seed fill, `lifetime_cull` mixed-lifetime partial/full expiry + empty-emitter guard + parallel parity without integration, rate-emit zero-rate/zero-dt/`at_capacity` no-ops, `simulate_step` integrated/culled counts + empty-emitter guard, parallel parity), emitter burst/rate emission (including `burst(0)`, returned emitted count, capacity clamp, burst+rate interleave), free-list slot recycling after partial expiry, attribute interpolation, drag integration, parallel vs single-thread simulation parity (grain boundaries, single particle, all-dead, multi-worker), GPU buffer layout alignment (per-column 16-byte offsets, padding, multi-capacity validation), simulate/emit dispatch counts (`forFrame`, block boundaries, `gridDimX`), CPU mirror pack/unpack round trips (full-capacity fill, undersized unpack guard), `spawn_effect` burst_count, effect instance duration, system spawn/update cleanup, and handle lifecycle without GPU or renderer dependencies.

`fuse_b7_vfx_gates_tests` (`ctest` name `fuse_b7_vfx_gates`, labels `perf;gate`) proves the B7.7 / B7.10 rows against independent references: exact per-second emission counts over 5 s, steady-state alive count vs `rate × lifetime`, ballistic motion vs the closed-form integrator and the continuous parabola, 4096 particles against an SDF sphere (no penetration, misses unaffected), plane-bounce apex `e² h`, 1-vs-8-worker bit-identical state, and a CPU step budget (enforced in NDEBUG builds).

## Build

Built with `FUSE_BUILD_CORE=ON`. Tests run when `FUSE_BUILD_CORE_TESTS=ON`.
