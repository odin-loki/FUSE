#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::cinematics {

enum class TrackKind {
    Transform2D,
    Transform3D,
    Camera,
    Audio,
    FxEvent,
};

/// Single timeline lane — ore analogue: Verve VTrack / VMotionTrack.
/// TODO(U5 extract): third_party/addons/Verve/Engine/source/Verve/Core/VTrack.h
/// TODO(U5 extract): Engine/source/Verve/ already in FUSE root — refactor here, do not re-merge.
struct Track {
    std::string name;
    TrackKind kind = TrackKind::Transform3D;
    float startTime = 0.f;
    float endTime = 0.f;
};

} // namespace fuse::cinematics
