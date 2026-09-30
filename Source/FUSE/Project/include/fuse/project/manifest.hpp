#pragma once

#include <fuse/hybrid/project_flags.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>

namespace fuse::project {

/// Versioned FUSE project manifest (maps to `project.json`).
struct DimensionSettings {
    bool enable3D = true;
    bool enable2D = true;
    bool enableUI = true;
};

struct ModuleSettings {
    bool ai = false;
    bool cinematics = false;
    bool fx = false;
    bool mechanics = false;
    bool adventure = false;
};

/// The only `project.json` schema this build reads and writes.
inline constexpr u32 kProjectSchemaVersion = 1u;

struct ProjectManifest {
    u32 schemaVersion = 0;
    std::string name;
    std::string projectRoot;
    DimensionSettings dimensions;
    ModuleSettings modules;
    std::string defaultWorld3D;
    std::string defaultWorld2D;
    /// Optional override for `jobs::computeWorkerCount()` (0 = platform default).
    u32 workerCap = 0;
};

enum class LoadStatus : u8 {
    Ok = 0,
    FileNotFound,
    ParseError,
    UnsupportedSchema,
};

struct LoadResult {
    LoadStatus status = LoadStatus::ParseError;
    ProjectManifest manifest;
    std::string error;
};

hybrid::DimensionFlags toDimensionFlags(const DimensionSettings& settings);

/// UNI-U6-FILE-1: `project.json` text for `manifest` (schemaVersion 1, dimensions, modules, default
/// worlds, workerCap) in the layout `parseManifest` reads back. `schemaVersion` is always written as
/// `kProjectSchemaVersion`; `projectRoot` is not stored (it is the file's directory). Strings are
/// JSON-escaped (quotes, backslashes, control characters).
std::string writeManifestJson(const ProjectManifest& manifest);

/// JSON string escaping / unescaping used by the manifest writer and loader.
std::string escapeJsonString(std::string_view text);
std::string unescapeJsonString(std::string_view text);

} // namespace fuse::project
