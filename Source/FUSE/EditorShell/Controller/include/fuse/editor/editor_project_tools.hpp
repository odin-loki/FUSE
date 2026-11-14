#pragma once
// Toolkit-free project tools of the editor: Project Settings (project.json edits), Import Asset (copy into
// assets/<kind>/ + cook on import with fuse_cook) and Build / Package (stage a project into a self-contained folder).
// Ports of the former Qt shell's ProjectSettingsDialog / importAssetsIntoProject / makeAssetCookJob / packageProject logic.
#include <fuse/project/manifest.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::editor {

// ---- processes ------------------------------------------------------------------------------------------------------
struct ProcessResult {
    bool started = false;
    bool finished = false; ///< false on timeout (the process is killed)
    int exitCode = -1;
    std::string output;    ///< stdout + stderr
};
/// Runs `program args...` in `workingDir` (empty: current) and waits up to `timeoutMs`.
ProcessResult runProcess(const std::string& program, const std::vector<std::string>& args, const std::string& workingDir = {},
                         u32 timeoutMs = 600000);
/// The running executable's directory.
std::string executableDir();

// ---- Project Settings -----------------------------------------------------------------------------------------------
struct ProjectSettingsLoad {
    bool loaded = false;
    project::ProjectManifest manifest;
    std::string error;
};
ProjectSettingsLoad loadProjectSettings(const std::string& projectRoot);
/// Writes project.json. Refuses an empty name and a project with neither 3D nor 2D (Qt rules).
bool saveProjectSettings(const std::string& projectRoot, project::ProjectManifest manifest, std::string* error = nullptr);

// ---- Import Asset ---------------------------------------------------------------------------------------------------
/// "textures", "models", "audio", "scenes", "scripts" or "misc" by file extension.
std::string assetImportKind(std::string_view extension);
/// Copies `sources` into `<projectRoot>/assets/<kind>/`; existing names get a `_N` suffix. Returns the project-relative
/// paths written ("assets/textures/wood_1.png").
std::vector<std::string> importAssetsIntoProject(const std::string& projectRoot, const std::vector<std::string>& sources,
                                                 std::string* error = nullptr);
struct AssetCookJob {
    std::string program;
    std::vector<std::string> arguments;
    std::string source; ///< project-relative source
    std::string output; ///< project-relative cooked output
};
/// FUSE_COOK, next to the executable, the build tree's Tools/FUSE, then PATH; empty when none.
std::string findCookTool();
/// Models -> .fusemesh, textures -> .fusetex, audio -> .fuseaudio under assets/cooked/<kind>/. False when `relPath`
/// has no cooker or `cookTool` is empty.
bool makeAssetCookJob(const std::string& projectRoot, const std::string& relPath, const std::string& cookTool, AssetCookJob& job);

// ---- Build / Package ------------------------------------------------------------------------------------------------
struct PackageOptions {
    std::string projectRoot;
    std::string outputDir;   ///< package goes to <outputDir>/<projectName>
    std::string runtimePath; ///< optional player executable copied into bin/
    bool cookAssets = true;
    bool clean = true;
    /// "Development" ships everything; "Shipping" also drops editor-only sidecars (prefab links, variants, backups).
    std::string configuration = "Development";
    std::vector<std::string> excludePatterns; ///< wildcards on the relative path or the file name ("*.psd", "docs/*")
    bool archive = false;                      ///< also write <outputDir>/<name>.tar.gz
};
struct PackageResult {
    bool ok = false;
    std::string packageDir;
    int copiedFiles = 0, cookedAssets = 0, failedCooks = 0, excludedFiles = 0;
    u64 totalBytes = 0;
    std::string archivePath;
    std::vector<std::string> cooked; ///< package-relative cooked outputs
    std::string error;
};
PackageResult packageProject(const PackageOptions& opts, const std::function<void(const std::string&)>& log = {});
std::vector<std::string> defaultPackageExcludes();
/// Case-insensitive wildcard match (* and ?; * also matches '/').
bool wildcardMatch(std::string_view pattern, std::string_view text);

} // namespace fuse::editor
