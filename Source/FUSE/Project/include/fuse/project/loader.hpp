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

/// New "Application (FUSE UI)" project: schema 2 `project.json` (kind app) + `ui/main.fuseui` from the template
/// (Templates/FuseUIApp holds the same files). Fails when `project.json` exists; keeps an existing main.fuseui. An
/// empty `name` uses the directory name.
SaveResult createAppProject(const std::string& projectDirectory, const std::string& name);

struct MigrateResult {
    bool ok = false;
    bool changed = false; ///< false when the file already was the newest schema
    u32 fromVersion = 0;
    std::string path;     ///< the project.json
    std::string text;     ///< the migrated file text
    std::string error;
};
/// `--migrate`: loads `path` (a project directory or project.json), migrates it to the newest schema and, when `write`
/// is set and something changed, rewrites the file.
MigrateResult migrateProjectFile(const std::string& path, bool write);

} // namespace fuse::project
