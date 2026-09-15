# Track B — VFX System (B7.7 deepen)

**Status:** B7.7 deepen — SoA free-list emission, jobified CPU simulation, GPU buffer/dispatch stubs  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.7  
**Source narrative:** [P7.md](../sources/P7.md) §7.7

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `ParticleSoA` | `Source/FUSE/VFX/include/fuse/vfx/particle_emitter.hpp` | SoA columns + `free_slots` dead-index pool |
| `particle_soa::*` | `Source/FUSE/VFX/include/fuse/vfx/particle_soa_ops.hpp` | Standalone burst/rate emit, `free_slot_count` / `sync_alive_count`, batch `recycle_slots`, `lifetime_cull`, and `simulate_step` CPU stubs |
| `ParticleGpuMirror` | `Source/FUSE/VFX/include/fuse/vfx/particle_gpu.hpp` | CPU mirror + packed SSBO layout for stub tests |
| `ParticleGpuDispatch` | `Source/FUSE/VFX/include/fuse/vfx/particle_gpu.hpp` | Simulate (256) / emit (64) CUDA grid counts |
| `ParticleEmitter` | `Source/FUSE/VFX/src/particle_emitter.cpp` | Burst/rate emit, gravity/drag/attribute integration |
| `ParticleSystem` | `Source/FUSE/VFX/include/fuse/vfx/particle_system.hpp` | Emitter/effect registry, frame `update` |
| `EffectInstance` | `Source/FUSE/VFX/include/fuse/vfx/effect_instance.hpp` | Timed effect playback and auto-destroy |

**Not in scope (deferred):** CUDA particle kernels, SDF collision, billboard rendering, wind fields, material binding.

---

## Design

### Emission

- **SoA helpers** — `particle_soa::burst_emit` and `accumulate_rate_emit` fill `ParticleSoA` directly with deterministic `seed` RNG (splitmix-style `mix_seed`). `BurstEmitResult::requested` / `emitted` report requested vs actual fill (clamped at capacity; zero-capacity / `burst_emit(0)` are no-ops). `free_slot_count` and `sync_alive_count` expose free-list and alive diagnostics. `ParticleEmitter` delegates to these helpers.
- **Burst** — emit `count` particles in one call; clamped to remaining free slots. `burst(0)` / `burst_emit(..., 0, ...)` and pre-`init` bursts are no-ops.
- **Rate** — accumulator driven by `emit_rate` (particles/second) during `simulate`; accumulator clears when the free list is exhausted at capacity.
- **Allocation** — `free_slots` provides O(1) slot lookup via `allocate_slot_()`; expired particles return slots after the simulate pass. `free_slot_count()` reports remaining capacity.
- **Spawn burst** — `ParticleSystem::spawn_effect(..., burst_count)` seeds the backing emitter with an immediate burst (default 1).

### Simulation

`particle_soa::lifetime_cull` ages live slots and recycles expired particles without integrating motion or attributes — a standalone stub for future GPU kill passes. `LifetimeCullResult::aged` / `culled` / `alive_after` report per-pass bookkeeping; `recycle_slots` batch-returns dead indices to the free list. `particle_soa::simulate_step` (used by `ParticleEmitter::simulate`) jobifies per-slot integration via `fuse::jobs::parallel_for` with grain size 64. Each live slot:

1. Advances normalized age (`age / lifetime`)
2. Applies gravity and drag to velocity
3. Integrates position
4. Interpolates size, RGB color, and alpha between start/end descriptor values

When the job scheduler is single-threaded (`workerCount == 0`), `parallel_for` runs the same body serially — tests assert parity between serial and multi-worker paths (1, 2, and 4 workers where the pool is available).

Edge cases covered in tests:

- Non-positive `dt` and disabled emitters skip integration without corrupting alive counts.
- Capacity not aligned to grain size (65 slots vs grain 64) still matches serial integration.
- All-dead emitters simulate safely with zero live slots.
- Partial slot recycle across multi-burst emit/expiry cycles.

### Effect instances

`ParticleSystem::spawn_effect` creates a backing emitter, bursts one particle, and registers an `EffectInstance` with optional duration. Finished effects destroy their emitters during `update`.

### GPU buffer layout (stub)

`ParticleGpuBufferLayout` packs the eight SoA columns (`positions` through `alive_flags`) into a single 16-byte-aligned device SSBO. Helpers expose per-column offsets, raw column byte totals, packing overhead, `columnDeviceAddress`, padding-after-column, alignment checks, and debug column names for stub validation. `ParticleGpuDispatch::forSimulate`, `forEmit`, and `forFrame` compute block counts for 256-wide simulate and 64-wide emit kernels; `isEmpty`, `hasSimLaunch`, `hasEmitLaunch`, `simCovers`, and `emitCovers` assert launch grids cover the requested slot/emit counts (including zero-work frames). `ParticleGpuBuffers::forCapacity` sizes the packed SSBO without device allocation. `ParticleGpuMirror` copies live CPU columns, packs/unpacks the device layout for unit tests, `syncAliveCountFromFlags`, `matchesPackedLayout`, and builds a `ParticleSoAGPU` pointer bundle — production simulation stays on the CPU reference path until CUDA kernels land.

---

## Build

`fuse_vfx` builds when `FUSE_BUILD_CORE=ON` (default). Tests register as `fuse_vfx_runtime`.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build --target fuse_vfx_tests
ctest --test-dir build -R fuse_vfx_runtime --output-on-failure
```

---

## Tests

| Test area | Coverage |
|-----------|----------|
| SoA ops | Direct `burst_emit` requested/emitted counts, `burst_emit(0)` and zero-capacity guards, `free_slot_count` / `sync_alive_count`, deterministic seed parity, `lifetime_cull` mixed-lifetime partial/full expiry + parallel parity, `simulate_step` age/kill, rate accumulator empty paths |
| Burst | Requested count, `burst(0)`, pre-init no-op, capacity clamp, slot recycling |
| Lifetime cull | Empty SoA, non-positive `dt` no-op, partial expiry without integration, full slot recycle |
| Emit rate | Steady-state particles/sec, burst+rate fill, capacity accumulator reset |
| Free list | Partial/mixed expiry recycle, multi-burst slot reuse, `free_slot_count()` after burst and simulate |
| Integration | Gravity, drag, size/color/alpha interpolation, disabled emitter |
| Parallel parity | 1/2/4-worker vs serial; grain boundary (65 slots); single particle; all-dead |
| GPU layout | Column offsets, raw/packed size helpers, 16-byte alignment at multiple capacities, padding, `ParticleGpuBuffers::forCapacity` |
| Dispatch | Simulate/emit block counts, `forFrame`, exact block boundaries, empty/zero-work dispatch, `simCovers` / `emitCovers`, `gridDimX` util |
| CPU mirror | Pack/unpack round trip, double-pack parity, `syncAliveCountFromFlags`, full-capacity fill, undersized unpack guard, `ParticleSoAGPU` pointer bundle |
| System | Spawn/update cleanup, `spawn_effect` burst_count, emitter handle lifecycle |

---

## Related

- Phase 7 checklist: [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md)
- Module README: [Source/FUSE/VFX/README.md](../../Source/FUSE/VFX/README.md)
