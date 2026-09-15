#pragma once

#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/serialiser.hpp>

#include <string>

namespace fuse::scene {

/// Resolved on-disk path for the project's default 3D world (.fuselevel).
std::string resolveDefaultWorldPath(const project::ProjectManifest& manifest);

/// Save/load scene via `project.json` `defaultWorld3D` relative path.
SerialiseResult saveForProject(const Scene& scene, const project::ProjectManifest& manifest);
SerialiseResult loadForProject(Scene& scene, const project::ProjectManifest& manifest);

SerialiseResult loadForProject(Scene& scene, const project::LoadResult& projectLoad);

} // namespace fuse::scene
