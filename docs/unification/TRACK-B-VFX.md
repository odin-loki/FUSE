# Track B — VFX System (B7.7 deepen)

**Status:** B7.7 deepen — SoA free-list emission, jobified CPU simulation landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.7  
**Source narrative:** [P7.md](../sources/P7.md) §7.7

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `ParticleSoA` | `Source/FUSE/VFX/include/fuse/vfx/particle_emitter.hpp` | SoA columns + `free_slots` dead-index pool |
| `ParticleEmitter` | `Source/FUSE/VFX/src/particle_emitter.cpp` | Burst/rate emit, gravity/drag/attribute integration |
| `ParticleSystem` | `Source/FUSE/VFX/include/fuse/vfx/particle_system.hpp` | Emitter/effect registry, frame `update` |
| `EffectInstance` | `Source/FUSE/VFX/include/fuse/vfx/effect_instance.hpp` | Timed effect playback and auto-destroy |

**Not in scope (deferred):** GPU/CUDA particle kernels, SDF collision, billboard rendering, wind fields, material binding.

---

## Design

### Emission

- **Burst** — emit `count` particles in one call; clamped to remaining free slots.
- **Rate** — accumulator driven by `emit_rate` (particles/second) during `simulate`.
- **Allocation** — `free_slots` provides O(1) slot lookup; expired particles return slots after the simulate pass.

### Simulation

`ParticleEmitter::simulate` jobifies per-slot integration via `fuse::jobs::parallel_for` with grain size 64. Each live slot:

1. Advances normalized age (`age / lifetime`)
2. Applies gravity and drag to velocity
3. Integrates position
4. Interpolates size, RGB color, and alpha between start/end descriptor values

When the job scheduler is single-threaded (`workerCount == 0`), `parallel_for` runs the same body serially — tests assert parity between serial and multi-worker paths.

### Effect instances

`ParticleSystem::spawn_effect` creates a backing emitter, bursts one particle, and registers an `EffectInstance` with optional duration. Finished effects destroy their emitters during `update`.

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
| Burst | Requested count, capacity clamp, slot recycling |
| Emit rate | Steady-state particles/sec approximation |
| Integration | Gravity, drag, size/color/alpha interpolation |
| Parallel parity | 4-worker vs single-thread simulation match |
| System | Spawn/update cleanup, emitter handle lifecycle |

---

## Related

- Phase 7 checklist: [TRACK-B-PHASE7.md](./TRACK-B-PHASE7.md)
- Module README: [Source/FUSE/VFX/README.md](../../Source/FUSE/VFX/README.md)
