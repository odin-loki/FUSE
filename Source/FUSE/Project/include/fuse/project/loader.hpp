#pragma once

#include <fuse/project/manifest.hpp>

#include <string>

namespace fuse::project {

LoadResult loadFromDirectory(const std::string& projectDirectory);
LoadResult loadFromFile(const std::string& projectJsonPath);
LoadResult parseManifest(std::string_view jsonText, const std::string& projectRoot);

} // namespace fuse::project
