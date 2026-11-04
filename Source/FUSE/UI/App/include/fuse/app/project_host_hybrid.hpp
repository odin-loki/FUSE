// fuse_app_project_hybrid: the engine game side for hybrid projects (ProjectHostDesc::gameSide). Links the hybrid
// composer, worlds and physics, so only hosts that run hybrid projects pay for them.
//
//   auto host = fuse::app::ProjectHost::start({.projectPath = dir, .gameSide = fuse::app::hybridGameSide()});
#pragma once

#include "fuse/app/project_host.hpp"

namespace fuse::app {

/// HybridComposer with the project's dimension flags, World3D (default world when its .fuselevel exists, physics on)
/// and World2D per the dimensions; each frame ticks + renders and hands out the composer's software frame. `handle`
/// returns the fuse::hybrid::HybridComposer* (e.g. composer->enableGpuScene() for a GPU-rendered viewport).
GameSideBackend hybridGameSide();

} // namespace fuse::app
