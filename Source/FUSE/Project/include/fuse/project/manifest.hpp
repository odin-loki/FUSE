#pragma once

#include <fuse/hybrid/project_flags.hpp>
#include <fuse/types.hpp>

#include <string>

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

struct ProjectManifest {
    u32 schemaVersion = 0;
    std::string name;
    std::string projectRoot;
    DimensionSettings dimensions;
    ModuleSettings modules;
    std::string defaultWorld3D;
    std::string defaultWorld2D;
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

} // namespace fuse::project
