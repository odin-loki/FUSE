# fuse_cinematics

Standalone cinematics timeline API distilled from **Verve** (MIT). This module does **not** link the legacy `Engine/source/Verve/` tree — it re-implements the playback kernel in `fuse::cinematics`.

## License / attribution

Verve is Copyright (C) 2014 Violent Tulip, licensed under the **MIT License**. See `third_party/addons/Verve/LICENSE` and the ore paths below.

## Ore map (Verve → fuse_cinematics)

| fuse type | Verve ore | Role |
|-----------|-----------|------|
| `Timeline` | `Engine/source/Verve/Core/VController.h` | Sequence director (time, duration, loop, play/pause/stop) |
| `Playhead` | `VController` time/status fields | Current ms position and playback state |
| `Track` | `Engine/source/Verve/Core/VTrack.h` | Ordered event lane, span, interpolation |
| `TimelineEvent` | `Engine/source/Verve/Core/VEvent.h` | Trigger time + duration keyframe |
| `TrackGroup` | `Engine/source/Verve/Core/VGroup.h` | Track grouping |

Submodule reference copy (same layout): `third_party/addons/Verve/Engine/source/Verve/Core/`.

## API sketch

```cpp
fuse::cinematics::Timeline timeline;
timeline.playhead().set_duration_ms(30'000);
auto& group = timeline.add_group("Director");
auto& track = group.add_track("Motion");
track.add_event({"intro", 2'000, 3'000});
timeline.play();
timeline.advance(100); // ms per tick
```

## Build

Enabled with `FUSE_BUILD_MODULES=ON` (default). Tests: `fuse_cinematics_tests` when `FUSE_BUILD_CORE_TESTS=ON`.
