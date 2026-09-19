#pragma once

// Ore: Verve VCameraTrack + VSceneObjectTrack sampled into U4 hybrid frame drives.

#include <fuse/cinematics/timeline.hpp>

namespace fuse::cinematics {

/// Sampled 2D sprite + proxy camera clear tint for hybrid demo / prestarter §10 gate.
struct HybridTimelineSample {
    float spriteX = 0.f;
    float spriteY = 0.f;
    float clearR = 0.1f;
    float clearG = 0.15f;
    float clearB = 0.25f;
};

/// Sample the first enabled camera + sprite tracks at the timeline playhead time.
HybridTimelineSample sample_hybrid_timeline_drive(const Timeline& timeline);

} // namespace fuse::cinematics
