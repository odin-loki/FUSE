#pragma once

// fuse_cinematics — distilled from Verve timeline ore (MIT).
// Ore: Engine/source/Verve/Core/VController.h (time scale, duration, loop)
//      third_party/addons/Verve/Engine/source/Verve/Core/VController.h

#include <fuse/types.hpp>

namespace fuse::cinematics {

/// Milliseconds on the sequence timeline (Verve uses S32 ms in VController).
using TimelineMs = s32;

/// Playback direction and speed multiplier (VController::mTimeScale).
using TimeScale = float;

enum class PlaybackState {
    Stopped,
    Playing,
    Paused,
};

enum class ControllerEvent {
    Init,
    Reset,
    Play,
    Pause,
    Stop,
    Loop,
};

} // namespace fuse::cinematics
