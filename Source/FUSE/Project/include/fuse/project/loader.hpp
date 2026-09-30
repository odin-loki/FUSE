#pragma once

#include <fuse/project/manifest.hpp>

#include <string>

namespace fuse::project {

LoadResult loadFromDirectory(const std::string& projectDirectory);
LoadResult loadFromFile(const std::string& projectJsonPath);
LoadResult parseManifest(std::string_view jsonText, const std::string& projectRoot);

/// UNI-U6-FILE-1 manifest writer result.
struct SaveResult {
    bool ok = false;
    std::string path; ///< the `project.json` written
    std::string error;
};

/// Writes `writeManifestJson(manifest)` to `projectJsonPath` (temporary file + rename, parent
/// directories created). `loadFromFile` on the result returns the same manifest (projectRoot = the
/// file's directory).
SaveResult saveToFile(const ProjectManifest& manifest, const std::string& projectJsonPath);
/// `saveToFile(manifest, projectDirectory + "/project.json")`; creates the directory.
SaveResult saveToDirectory(const ProjectManifest& manifest, const std::string& projectDirectory);

} // namespace fuse::project
