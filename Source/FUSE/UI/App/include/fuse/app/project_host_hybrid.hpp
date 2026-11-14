// fuse_app_project_hybrid: the engine game side for hybrid projects (ProjectHostDesc::gameSide). Links the hybrid
// composer, worlds and physics, so only hosts that run hybrid projects pay for them.
//
//   auto host = fuse::app::ProjectHost::start({.projectPath = dir, .gameSide = fuse::app::hybridGameSide()});
#pragma once

#include "fuse/app/project_host.hpp"

namespace fuse::app {

/// HybridComposer with the project's dimension flags, World3D (default world when its .fuselevel exists, physics on)
/// and World2D per the dimensions; each frame ticks + renders and hands out the composer's software frame. With the
/// Vulkan RHI it also offers the GPU viewport (GameSideBackend::gpu*): a device created at start, the worlds rendered
/// by a HybridSceneRenderer on it into its headless target, sampled in place by GPU drawn windows. `handle` returns
/// the fuse::hybrid::HybridComposer* (its worlds: composer->world3D() / world2D()).
GameSideBackend hybridGameSide();

} // namespace fuse::app
