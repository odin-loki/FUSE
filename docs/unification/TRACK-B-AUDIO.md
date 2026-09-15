# Track B — Spatial Audio Engine (B7.2 deepen)

**Status:** B7.2 deepen — bus routing tests, HRTF-lite pan edge cases, occlusion stub landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.2  
**Source narrative:** [P7.md](../sources/P7.md) §7.2

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `AttenuationCurve` / `AttenuationParams` | `Source/FUSE/Audio/include/fuse/audio/attenuation.hpp` | Linear, logarithmic, exponential distance falloff |
| `compute_attenuation` | `Source/FUSE/Audio/src/attenuation.cpp` | Legacy 3-arg overload + curve-aware overload |
| `ListenerBasis` / `to_listener_space` | `Source/FUSE/Audio/include/fuse/audio/math.hpp` | World → listener-local transform for panning |
| `AudioBus` / `AudioBusMixer` | `Source/FUSE/Audio/include/fuse/audio/audio_bus.hpp` | Per-category gain stub (Master, Sfx, Music, Voice) |
| `OcclusionParams` / `evaluate_occlusion_gain` | `Source/FUSE/Audio/include/fuse/audio/occlusion.hpp` | Visibility → gain stub; AABB line-of-sight blocker |
| `SpatialMixer` | `Source/FUSE/Audio/include/fuse/audio/spatial_mixer.hpp` | CPU HRTF-lite pan + curve attenuation + bus routing + occlusion |
| `AudioEngine` | `Source/FUSE/Audio/include/fuse/audio/audio_engine.hpp` | OpenAL backend sync, CUDA/CPU reverb facade, `bus_mixer()` accessor |

**Not in scope (deferred):** HRTF impulse-response files, per-source bus sends, ducking/sidechain, real-time CUDA FFT reverb on device, OGG decode, ECS system wiring, dynamic occlusion raycasts.

---

## Design

### Attenuation curves

`AudioSourceDesc` carries `attenuation` (`Linear`, `Logarithmic`, `Exponential`), `min_distance`, `max_distance`, and `rolloff`. Curves mirror Torque `SFXDistanceModel` semantics:

| Curve | Behaviour |
|-------|-----------|
| **Linear** | `min / (min + (d - min))` — reaches zero at `max_distance` |
| **Logarithmic** | `min / (min + rolloff * (d - min))` — inverse-distance style |
| **Exponential** | `(d / min)^(-rolloff)` — steeper falloff at distance |

### Listener orientation

`AudioListener` exposes `forward` and `up`. `SpatialMixer` builds a right-handed `ListenerBasis` and transforms each source offset into listener-local space before computing azimuth pan. OpenAL backend orientation is synced from the same vectors in `AudioEngine::update`.

### Bus gains (stub)

`AudioBusMixer` holds per-bus gain multipliers. Effective output for a source is:

```
effective = listener.master_volume * bus_mixer.effective_gain(source.bus)
```

`effective_gain(bus)` returns `master_gain * bus_gain` for non-master buses. Routing is stub-only — no sub-mix buffers yet. `AudioEngine::bus_mixer()` exposes the mixer for category-level gain control.

### HRTF-lite panning

`apply_hrtf_pan` computes azimuth from listener-local offset and applies equal-power L/R gains via `sin(azimuth)`. Edge cases:

| Case | Behaviour |
|------|-----------|
| Co-located source (`distance < ε`) | Mono centre — no pan split |
| Ahead / behind on forward axis | Near-centre pan |
| Left / right offset | Asymmetric L/R energy |
| `hrtf_enabled = false` | Equal L/R regardless of position |

### Occlusion (stub)

`AudioSourceDesc::occlusion` is a visibility factor in `[0, 1]`. `evaluate_occlusion_gain` maps visibility to gain with a `min_gain` floor (default 0.1). `compute_blocker_visibility` performs a segment-vs-AABB intersection stub for line-of-sight testing. No HF filtering yet — `OcclusionParams::hf_attenuation` is reserved.

---

## Build

`fuse_audio` builds when `FUSE_BUILD_AUDIO=ON` (default). Tests register as `fuse_audio_b72`.

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_AUDIO=ON

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_audio
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_AUDIO=OFF` | `fuse_audio` library omitted |
| `FUSE_AUDIO_ENABLE_OPENAL=OFF` | Null audio backend (CI-safe) |
| OpenAL not found | Falls back to vendored openal-soft or null backend |

---

## Tests

`Source/FUSE/Audio/tests/test_audio_engine.cpp` (`fuse_audio_b72`):

| Test | Gate |
|------|------|
| `testAttenuationCurves` | Logarithmic and exponential curves attenuate mid-range |
| `testListenerOrientationTransform` | `to_listener_space` maps axes correctly |
| `testBusGains` | `effective_gain` multiplies master × bus |
| `testBusRoutingAllCategories` | Music and Voice buses route through master |
| `testBusGainClampsNegative` | Negative gains clamp to zero |
| `testBusRoutingAffectsMixOutput` | Bus gain scales spatial mix energy |
| `testHrtfPanEdgeCases` | Ahead/left/right/behind/co-located pan; HRTF-off is mono |
| `testOcclusionStub` | Visibility mapping and AABB blocker stub |
| `testOcclusionReducesMixOutput` | Occluded source is quieter with min_gain floor |
| `testSpatialPanRespectsListenerOrientation` | L/R asymmetry changes when listener rotates |
| `testSpatialAttenuationAtMaxDistance` | Source at `max_distance` is near-silent |
| `testThirtyTwoSourcesMixWithoutNaN` | 32 spatial sources mix without NaN |

---

## Gates (B7.2 deepen)

- [x] Attenuation curves: linear, logarithmic, exponential
- [x] Listener orientation affects spatial pan
- [x] Bus gain stub with master × category routing
- [x] Bus routing tests cover all categories and mix output scaling
- [x] HRTF-lite pan edge cases (ahead, lateral, behind, co-located, disabled)
- [x] Occlusion stub with visibility gain and AABB blocker
- [x] `fuse_audio_b72` CTest target green
- [x] No owning raw pointers in public FUSE APIs

---

## References

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.2
- [P7.md](../sources/P7.md) §7.2
- Torque `SFXDistanceModel` in `Engine/source/sfx/sfxCommon.h`
