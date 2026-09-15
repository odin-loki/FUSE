# U5 Module: fuse_cinematics

**Status:** Deepened Verve track kernel (camera / sprite / property stubs)  
**Target:** `Source/FUSE/Modules/cinematics/`  
**Ore:** Verve MIT — `Engine/source/Verve/Core/` and `third_party/addons/Verve/Engine/source/Verve/Core/`

## Harvested concepts

- **Timeline** — from `VController` (play/pause/stop, duration, time scale, loop, `processTick` advance)
- **Playhead** — current ms position and playback state
- **Track** — from `VTrack` (sorted events, span, `calculateInterp`)
- **TimelineEvent** — from `VEvent` (trigger time, duration, active span)
- **TrackGroup** — from `VGroup`
- **CameraTrack** — from `VCameraTrack` / `VSceneObjectTrack` (camera keyframes, `sample_at`)
- **SpriteTrack** — from `VSceneObjectTrack` (2D sprite transform stub)
- **PropertyTrack** — from `VMotionTrack` (scalar property rail stub)
- **interpolate** — `lerp`, `lerp_vec3`, `apply_ease`, `calculate_track_interp` (Verve `calculateInterp`)

## Not in scope (later PRs)

- Torque bridge types (`TCamera`, `VMotionTrack`, …)
- `VPath` / `VActor` rails
- Legacy Gui `VTimeLineControl` (replaced by Qt editor pane U6)
- GMK cutscene overlap → `fuse_mechanics` boundary
- Hybrid demo camera/sprite drive (gate: 30s timeline moves camera + sprite)

## Build / test

- CMake: `FUSE_BUILD_MODULES=ON` (default) builds `fuse_cinematics`
- CTest: `fuse_cinematics_tests` — 30s timeline advance, track span, interpolation, typed track sampling, interpolate helpers

## License

Verve (Violent Tulip, 2014) — MIT. See `third_party/addons/Verve/LICENSE`.
