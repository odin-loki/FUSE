# U5 Module: fuse_cinematics

**Status:** Deepened Verve track kernel (camera / sprite / property / audio / event stubs, scrub + cue queue)  
**Target:** `Source/FUSE/Modules/cinematics/`  
**Ore:** Verve MIT — `Engine/source/Verve/Core/` and `third_party/addons/Verve/Engine/source/Verve/Core/`

## Harvested concepts

- **Timeline** — from `VController` (play/pause/stop, duration, time scale, loop, `processTick` advance)
- **Playhead** — current ms position, playback state, `scrub_to` seek without starting playback
- **Track** — from `VTrack` (sorted events, span, `calculateInterp`)
- **TimelineEvent** — from `VEvent` (trigger time, duration, active span)
- **TrackGroup** — from `VGroup`
- **CameraTrack** — from `VCameraTrack` / `VSceneObjectTrack` (position/FOV/roll keyframes, `keyframe_span`, `sample_at`, `empty`)
- **CameraSample** — sampled pose with `look_direction()` / `look_distance()` helpers
- **LookAtResolver** — entity-bound look-at stub (`CameraLookAtMode::TargetEntity`) without Torque `setTrackObject`; falls back to fixed `look_at` when unresolved
- **camera_look_direction** / **camera_look_distance** — world-space aim helpers for sampled or manual poses
- **lerp_fov** — clamped vertical-FOV interpolation for camera keyframe rails (`kMinFovDeg`..`kMaxFovDeg`)
- **SpriteTrack** — from `VSceneObjectTrack` (2D sprite transform stub)
- **PropertyTrack** — from `VMotionTrack` (scalar property rail stub)
- **AudioTrack** — from `VSoundEffectTrack` (sound asset id + volume keyframes stub)
- **EventTrack** — from `VScriptEventTrack` (script hook id + cue events)
- **CueQueue** — forward-crossed event dispatch during `scrub_to` / `advance` (game-thread `drain`)
- **CuePayload** — typed stub payloads (`ScriptHook`, `AudioClip`, `Custom`) attached to each `CueEntry`
- **Consume-once ledger** — `Timeline` tracks fired cue keys; loop / `reset` clears so cues re-arm
- **interpolate** — `lerp`, `lerp_vec3`, `apply_ease`, `calculate_track_interp` (Verve `calculateInterp`)

## Not in scope (later PRs)

- Torque bridge types (`TCamera`, `VMotionTrack`, …)
- `VPath` / `VActor` rails
- Legacy Gui `VTimeLineControl` (replaced by Qt editor pane U6)
- GMK cutscene overlap → `fuse_mechanics` boundary
- Hybrid demo camera/sprite drive (gate: 30s timeline moves camera + sprite)

## Build / test

- CMake: `FUSE_BUILD_MODULES=ON` (default) builds `fuse_cinematics`
- CTest: `fuse_cinematics_tests` — 30s timeline advance, track span, interpolation, typed track sampling (camera FOV extremes/roll/look-at direction/empty track), playhead scrub, cue queue drain, advance/scrub cue enqueue, scrub order, consume-once, loop reset, empty timeline, cue payload stubs

## License

Verve (Violent Tulip, 2014) — MIT. See `third_party/addons/Verve/LICENSE`.
