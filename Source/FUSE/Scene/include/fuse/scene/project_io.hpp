#pragma once

#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/serialiser.hpp>

#include <string>

namespace fuse::scene {

/// Resolved on-disk path for the project's default 3D world (.fuselevel).
std::string resolveDefaultWorldPath(const project::ProjectManifest& manifest);

/// Resolved on-disk path for the project's default 2D world (.fuselevel).
std::string resolveDefaultWorld2DPath(const project::ProjectManifest& manifest);

/// Save/load scene via `project.json` `defaultWorld3D` relative path.
SerialiseResult saveForProject(const Scene& scene, const project::ProjectManifest& manifest);
SerialiseResult loadForProject(Scene& scene, const project::ProjectManifest& manifest);

SerialiseResult loadForProject(Scene& scene, const project::LoadResult& projectLoad);

/// UNI-U7-WORLD-1: `.fuselevel` v3 (scene + full ECS registry) for the project's default world of
/// `dimension` (`defaultWorld3D` / `defaultWorld2D`). Save creates the parent directory.
SerialiseResult saveWorldForProject(const Scene& scene, const ecs::Registry& registry,
                                    const project::ProjectManifest& manifest,
                                    SceneDimension dimension = SceneDimension::World3D);
SerialiseResult loadWorldForProject(Scene& scene, ecs::Registry& registry, const project::ProjectManifest& manifest,
                                    SceneDimension dimension = SceneDimension::World3D, SceneFileInfo* info = nullptr);

} // namespace fuse::scene
