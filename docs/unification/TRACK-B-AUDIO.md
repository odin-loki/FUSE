# Track B — Spatial Audio Engine (B7.2 deepen)

**Status:** B7.2 deepen — bus routing cycle guard, attenuation endpoint stubs, empty custom curve  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.2  
**Source narrative:** [P7.md](../sources/P7.md) §7.2

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `AttenuationCurve` / `AttenuationParams` | `Source/FUSE/Audio/include/fuse/audio/attenuation.hpp` | Linear, logarithmic, exponential, inverse, custom keypoints |
| `compute_attenuation` / `sample_attenuation_curve` | `Source/FUSE/Audio/src/attenuation.cpp` | Legacy 3-arg overload + curve-aware sampling with clamp |
| `make_attenuation_params` / `sample_attenuation_at_*` | `Source/FUSE/Audio/src/attenuation.cpp` | Build params from `AudioSourceDesc`; min/max endpoint sample stubs |
| `ListenerBasis` / `to_listener_space` | `Source/FUSE/Audio/include/fuse/audio/math.hpp` | World → listener-local transform; safe fallbacks for degenerate forward/up |
| `AudioBus` / `AudioBusMixer` | `Source/FUSE/Audio/include/fuse/audio/audio_bus.hpp` | Per-category gain stub with parent-chain routing, cycle guard, `reset_gains` |
| `OcclusionParams` / `evaluate_occlusion_*` | `Source/FUSE/Audio/include/fuse/audio/occlusion.hpp` | Visibility → LF/HF gain stubs; segment-vs-AABB ray + blocker factor 0..1 |
| `PanLaw` / `sample_pan_law` | `Source/FUSE/Audio/include/fuse/audio/binaural_pan.hpp` | Equal-power and linear stereo pan law curves |
| `BinauralPanParams` / `compute_binaural_*` | `Source/FUSE/Audio/include/fuse/audio/binaural_pan.hpp` | Listener-local azimuth/elevation, selectable pan law, ITD stub, distance blend |
| `ReverbZoneParams` / `blend_reverb_zones` | `Source/FUSE/Audio/include/fuse/audio/reverb_zones.hpp` | Zone AABB membership + overlapping wet/dry blend |
| `SpatialMixer` | `Source/FUSE/Audio/include/fuse/audio/spatial_mixer.hpp` | CPU HRTF-lite pan + curve attenuation + bus routing + blocker occlusion |
| `AudioEngine` | `Source/FUSE/Audio/include/fuse/audio/audio_engine.hpp` | OpenAL backend sync, CUDA/CPU reverb facade, zone blend + occlusion blockers |

**Not in scope (deferred):** HRTF impulse-response files, per-source bus sends, ducking/sidechain, real-time CUDA FFT reverb on device, OGG decode, ECS system wiring, dynamic occlusion raycasts, HF IIR/LPF filtering.

---

## Design

### Attenuation curves

`AudioSourceDesc` carries `attenuation` (`Linear`, `Logarithmic`, `Exponential`), `min_distance`, `max_distance`, and `rolloff`. Curves mirror Torque `SFXDistanceModel` semantics:

| Curve | Behaviour |
|-------|-----------|
| **Linear** | `min / (min + (d - min))` — reaches zero at `max_distance` |
| **Logarithmic** | `min / (min + rolloff * (d - min))` — inverse-distance clamped style |
| **Exponential** | `(d / min)^(-rolloff)` — steeper falloff at distance |
| **Inverse** | `min / (rolloff * d)` — pure inverse-distance sample |
| **Custom** | Piecewise-linear interpolation over sorted `(distance, gain)` keypoints; empty keypoint list samples unity |

`make_attenuation_params(AudioSourceDesc)` centralises param construction (including custom keypoints). `sample_attenuation_at_min` / `sample_attenuation_at_max` expose endpoint stubs for unit tests.

### Listener orientation

`AudioListener` exposes `forward` and `up`. `SpatialMixer` builds a right-handed `ListenerBasis` via `make_listener_basis_safe`, which falls back to default forward/up when vectors are zero or parallel. `is_listener_orientation_valid`, `sanitize_listener_forward`, and `sanitize_listener_up` centralise edge-case handling. OpenAL backend orientation is synced from the same vectors in `AudioEngine::update`.

### Pan law curves

`BinauralPanParams::pan_law` selects the ILD stub curve applied after azimuth is mapped to a pan position in `[-1, 1]`:

| Law | L/R mapping |
|-----|-------------|
| **EqualPower** | `sqrt(0.5 * (1 ∓ pan))` — constant-power centre |
| **Linear** | `0.5 * (1 ∓ pan)` — amplitude pan |

`sample_pan_law` and `clamp_pan_position` expose the curves for unit tests and future mixer wiring.

### HRTF-lite panning

`apply_hrtf_pan` delegates to `compute_binaural_pan_gains` for ILD (selected pan law) and a Woodworth-style ITD stub (`max_itd_seconds * sin(azimuth)`). `apply_hrtf_distance_factor` narrows the binaural image when distance attenuation is low. Edge cases:

| Case | Behaviour |
|------|-----------|
| Co-located source (`distance < ε`) | Mono centre — no pan split |
| Ahead / behind on forward axis | Near-centre pan; ITD stub ≈ 0 |
| Left / right offset | Asymmetric L/R energy; signed ITD stub |
| Elevated source | Both ears scaled by `elevation_rolloff` stub |
| `hrtf_enabled = false` | Equal L/R regardless of position |
| No listener entity | World-relative pan; unity master gain |
| Degenerate forward/up | Safe basis falls back to default orientation |
| Low distance attenuation | `min_spatial_blend` narrows L/R spread toward mono |

### Bus gains (stub)

`AudioBusMixer` holds per-bus gain multipliers and optional parent routing (default: Sfx/Music/Voice → Master). Effective output for a source is:

```
effective = bus_mixer.effective_output_gain(source.bus, listener.master_volume)
```

`routed_bus_gain(bus)` multiplies gains along the parent chain (cycle-safe hop limit); `effective_gain(bus)` includes master; `effective_output_gain(bus, listener_master_volume)` applies listener master with clamp. `reset_gains()` restores unity gains and default Sfx/Music/Voice → Master routing. `clamp_bus_gain` keeps stub gains in `[0, 1]`. Routing is stub-only — no sub-mix buffers yet. `SpatialMixer` uses `effective_output_gain` for per-source output scaling; `AudioEngine::bus_mixer()` exposes the mixer for category-level gain control.

### Occlusion (stub)

`AudioSourceDesc::occlusion` is a per-source visibility factor in `[0, 1]`. `SpatialMixer::set_occlusion_blockers` registers world-space AABB blockers; `compute_source_visibility` centralises effective visibility:

```
blocker_factor = compute_blockers_factor(listener, source, blockers)   // 0 = clear, 1 = blocked
visibility     = clamp(source.occlusion) * (1 - blocker_factor)
```

Occlusion helpers map visibility to attenuation multipliers:

| Helper | Behaviour |
|--------|-----------|
| `evaluate_occlusion_gain` | LF gain with `min_gain` floor (default 0.1) |
| `evaluate_occlusion_hf_gain` | HF rolloff stub — lerp toward `hf_attenuation` (default 0.6) |
| `evaluate_occlusion_attenuation` | Bundles LF + HF gains for mixer consumption |
| `segment_intersects_aabb` | Segment-vs-AABB ray stub for blocker geometry |
| `compute_blocker_visibility` | Segment-vs-AABB ray stub; returns `blocked_visibility` (default 0.25) |
| `compute_blockers_visibility` | Minimum visibility across multiple blocker AABBs |
| `compute_blocker_factor` | Inverse occlusion amount in `[0, 1]` — 0 = clear LOS, 1 = fully blocked |
| `compute_blockers_factor` | Maximum blocker factor across multiple AABBs |
| `SpatialMixer::compute_source_visibility` | Combines per-source occlusion with registered blockers |

`SpatialMixer` and `AudioEngine::sync_backend_sources_` multiply distance attenuation by `occlusion.gain * occlusion.hf_gain` before panning/backend gain. No HF filter yet — `hf_gain` is a scalar energy stub.

### Reverb zones (stub)

`ReverbZone` carries an AABB `bounds`, `wet_dry` (mix ratio), and `send_level` (bus send scalar). `listener_in_reverb_zone` tests membership; `blend_reverb_zones` averages wet/dry and send across all zones containing the listener.

Effective wet contribution when the listener is inside at least one zone:

```
blend     = blend_reverb_zones(listener, zones)
wet_mix   = blend.wet_dry * blend.send_level
mixed     = dry * (1 - wet_mix) + convolved * wet_mix
```

When the listener is outside all zones, reverb is skipped (fully dry). `AudioEngine::set_reverb_send_level` / `reverb_send_level()` adjust zone send without re-adding zones. Convolution runs through `ConvReverbCpu` (CUDA facade deferred).

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
| `testBusGainClampsAboveUnity` | Gains above unity clamp to one |
| `testBusChainRouting` | Parent-chain `routed_bus_gain` / `effective_output_gain` |
| `testBusMasterParentLocked` | Master bus parent cannot be rerouted |
| `testBusRoutingCycleGuard` | Cyclic parent chains terminate safely |
| `testBusEffectiveOutputGainEndpoints` | Listener master volume 0/1 endpoints |
| `testBusResetGains` | `reset_gains` restores defaults |
| `testEmptyCustomAttenuationCurve` | Custom curve with zero keypoints samples unity |
| `testMakeAttenuationParamsFromDesc` | `make_attenuation_params` copies desc fields |
| `testAttenuationCurveSampleEndpoints` | Min/max endpoint stubs for all curve types |
| `testAttenuationCurveExtremes` | Min/max distance, linear/inverse/custom curve extremes |
| `testAttenuationGainClamp` | Custom keypoint gains clamp to `[0, 1]` |
| `testCustomAttenuationCurveAffectsMix` | Custom keypoints wired through `SpatialMixer` |
| `testBusRoutingAffectsMixOutput` | Bus gain scales spatial mix energy |
| `testHrtfPanEdgeCases` | Ahead/left/right/behind/co-located pan; HRTF-off is mono |
| `testBinauralPanFrontBackSideExtremes` | Front/back centre pan; lateral ITD sign; side ILD asymmetry |
| `testBinauralPanListenerBasisTransform` | World offset → listener-local azimuth/elevation |
| `testBinauralPanGainClamp` | Per-ear gains clamp to [0, 1] |
| `testBinauralPanDistanceFactorNarrowsImage` | Attenuation narrows binaural spread via `apply_hrtf_distance_factor` |
| `testPanLawCurveEndpoints` | Equal-power and linear pan law endpoints and clamp |
| `testBinauralPanAzimuthEndpoints` | Hard left/right azimuth maps to pan endpoints |
| `testListenerOrientationEdgeCases` | Degenerate forward/up sanitization and safe basis |
| `testEmptyListenerSpatialMix` | No listener entity mixes without crash; world-relative pan |
| `testOcclusionStub` | LF/HF gain mapping, attenuation bundle, multi-blocker visibility |
| `testBlockerFactorExtremes` | Ray/AABB intersection, blocker factor 0..1, mixer visibility helper |
| `testOcclusionFactorExtremes` | Unity vs floored effective occlusion gain; blocker visibility extremes |
| `testOcclusionReducesMixOutput` | Occluded source is quieter with min_gain floor |
| `testOcclusionBlockerAttenuatesMix` | AABB blocker on LOS reduces spatial mix energy |
| `testReverbZoneMembership` | Listener inside/outside zone AABB |
| `testReverbZoneBlendExtremes` | Dry/wet extremes and clamped zone parameters |
| `testReverbZoneOverlappingBlend` | Overlapping zones average wet/dry; outside yields dry |
| `testReverbZoneListenerPositionAffectsMix` | Inside zone is wetter than outside |
| `testReverbSendLevelDryMix` | Zero `wet_dry` leaves dry mix unchanged |
| `testReverbSendLevelWetMix` | Full send increases mix energy vs dry |
| `testReverbSendLevelScalesMixOutput` | `set_reverb_send_level` scales wet contribution |
| `testSpatialPanRespectsListenerOrientation` | L/R asymmetry changes when listener rotates |
| `testSpatialAttenuationAtMaxDistance` | Source at `max_distance` is near-silent |
| `testThirtyTwoSourcesMixWithoutNaN` | 32 spatial sources mix without NaN |

---

## Gates (B7.2 deepen)

- [x] Attenuation curves: linear, logarithmic, exponential
- [x] Listener orientation affects spatial pan
- [x] Bus gain stub with master × category routing
- [x] Bus parent-chain routing (`routed_bus_gain`, `effective_output_gain`, cycle guard, `reset_gains`)
- [x] Attenuation endpoint stubs (`sample_attenuation_at_min/max`, empty custom curve)
- [x] Inverse and custom keypoint attenuation curves sampled in spatial path
- [x] Bus routing tests cover all categories, chain, clamp, and mix output scaling
- [x] HRTF-lite pan edge cases (ahead, lateral, behind, co-located, disabled)
- [x] Binaural pan helpers: azimuth/elevation, pan law curves, ITD/ILD stubs, gain clamp, distance blend
- [x] Listener orientation helpers: validity check, sanitization, safe basis for degenerate input
- [x] Pan endpoint, empty-listener, and orientation edge-case tests
- [x] Occlusion segment-vs-AABB blocker factor 0..1 wired into spatial attenuation + backend sync
- [x] Reverb zone AABB membership + overlapping blend + listener-scoped wet/dry
- [x] `fuse_audio_b72` CTest target green
- [x] No owning raw pointers in public FUSE APIs
- [ ] Per-source reverb sends (follow-up)
- [ ] HF occlusion IIR/LPF filter (follow-up)
- [ ] Real-time CUDA FFT reverb on device (follow-up)

---

## References

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.2
- [P7.md](../sources/P7.md) §7.2
- Torque `SFXDistanceModel` in `Engine/source/sfx/sfxCommon.h`
