# U5 Module: fuse_cinematics

**Status:** Initial extraction (Verve timeline kernel)  
**Target:** `Source/FUSE/Modules/cinematics/`  
**Ore:** Verve MIT — `Engine/source/Verve/Core/` and `third_party/addons/Verve/Engine/source/Verve/Core/`

## Harvested concepts

- **Timeline** — from `VController` (play/pause/stop, duration, time scale, loop, `processTick` advance)
- **Playhead** — current ms position and playback state
- **Track** — from `VTrack` (sorted events, span, `calculateInterp`)
- **TimelineEvent** — from `VEvent` (trigger time, duration, active span)
- **TrackGroup** — from `VGroup`

## Not in scope (later PRs)

- Torque bridge types (`TCamera`, `VMotionTrack`, …)
- `VPath` / `VActor` rails
- Legacy Gui `VTimeLineControl` (replaced by Qt editor pane U6)
- GMK cutscene overlap → `fuse_mechanics` boundary

## Build / test

- CMake: `FUSE_BUILD_MODULES=ON` (default) builds `fuse_cinematics`
- CTest: `fuse_cinematics_tests` — 30s timeline advance, track span, interpolation

## License

Verve (Violent Tulip, 2014) — MIT. See `third_party/addons/Verve/LICENSE`.
