#pragma once

// Ore: Engine/source/Verve/Core/VTrack.cpp (calculateInterp, _calculateInterp)
//      third_party/addons/Verve/Engine/source/Verve/Core/VTrack.cpp

#include <fuse/cinematics/event.hpp>
#include <fuse/cinematics/types.hpp>

#include <vector>

namespace fuse::cinematics {

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

enum class EaseMode {
    Linear,
    SmoothStep,
};

float clamp01(float t);
float lerp(float a, float b, float t);
Vec3 lerp_vec3(const Vec3& a, const Vec3& b, float t);

float apply_ease(EaseMode mode, float t);

/// Verve VTrack::_calculateInterp — normalized 0..1 position along event gaps.
float calculate_track_interp(const std::vector<TimelineEvent>& events,
                             TimelineMs time_ms,
                             TimelineMs sequence_duration_ms);

/// Verve VTrack::calculateInterp — mirrors reverse-playback inversion.
float calculate_track_interp_forward(bool playing_forward,
                                     const std::vector<TimelineEvent>& events,
                                     TimelineMs time_ms,
                                     TimelineMs sequence_duration_ms);

} // namespace fuse::cinematics
