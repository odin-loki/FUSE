#pragma once
// Shared T0 frame tuning (Lavapipe gates in test_rp_frame.cpp and the hybrid runtime defaults).
#include <fuse/renderer/frame/frame_composer.hpp>

namespace fuse::renderer::frame {

/// Modest VSM / DDGI / atmosphere / fog / cloud sizes for software Vulkan and desktop preview.
void applyLavapipeComposerDefaults(FrameComposerDesc& desc);

/// Full T0 stage stack with smaller LUTs and grids (matches fuse_rp_frame `baseSettings()`).
void applyLavapipeTier0FrameSettings(FrameSettings& settings, u32 renderWidth, u32 renderHeight);

} // namespace fuse::renderer::frame
