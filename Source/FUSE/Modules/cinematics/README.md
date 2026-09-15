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
| `CameraTrack` | `Engine/source/Verve/Extension/Camera/VCameraTrack.h` | Camera keyframe rail + sampling stub |
| `SpriteTrack` | `Engine/source/Verve/Extension/SceneObject/VSceneObjectTrack.h` | 2D sprite transform stub |
| `PropertyTrack` | `Engine/source/Verve/Extension/Motion/VMotionTrack.h` | Scalar property rail stub |
| `AudioTrack` | `Engine/source/Verve/Extension/SoundEffect/VSoundEffectTrack.h` | Sound-effect lane + volume keyframes stub |
| `EventTrack` | `Engine/source/Verve/Extension/Script/VScriptEventTrack.h` | Script/director cue lane stub |
| `CueQueue` | Verve track event dispatch (editor scrub + advance) | Pending cue buffer for game-thread drain |
| `interpolate` | `Engine/source/Verve/Core/VTrack.cpp` | `lerp`, easing, `calculateInterp` helpers |

Submodule reference copy (same layout): `third_party/addons/Verve/Engine/source/Verve/Core/`.

## API sketch

```cpp
fuse::cinematics::Timeline timeline;
timeline.playhead().set_duration_ms(30'000);
auto& group = timeline.add_group("Director");

auto& camera = group.add_camera_track("MainCam");
camera.set_target_camera_id("player_cam");
camera.add_keyframe({0, {0, 0, 5}, {0, 0, 0}, 60.f});
camera.add_keyframe({2'000, {0, 0, 10}, {0, 0, 0}, 45.f});

auto& sprite = group.add_sprite_track("Hero");
sprite.add_keyframe({0, 0.f, 0.f, 1.f});
sprite.add_keyframe({1'000, 100.f, 50.f, 0.f});

auto& cues = group.add_event_track("Director");
cues.add_event({"door_open", 1'000});

timeline.scrub_to(2'000); // enqueues forward-crossed cues into timeline.cue_queue()
const auto pending = timeline.cue_queue().drain();

timeline.play();
timeline.advance(100); // ms per tick
```

## Build

Enabled with `FUSE_BUILD_MODULES=ON` (default). Tests: `fuse_cinematics_tests` when `FUSE_BUILD_CORE_TESTS=ON`.
