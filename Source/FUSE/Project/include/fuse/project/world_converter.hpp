#pragma once

#include <fuse/project/manifest.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

enum class ConvertStatus : u8 {
    Ok = 0,
    IoError,
    ParseError,
    UnsupportedSource,
};

struct ConvertResult {
    ConvertStatus status = ConvertStatus::IoError;
    std::string outputPath;
    u32 entityCount = 0;
    u32 wiringStubCount = 0;
    std::string note;
};

/// Convert a T3D `.mis` mission into a minimal `.fuselevel` scene file (U7).
ConvertResult convertT3DMissionToFuselevel(const std::string& missionPath,
                                           const std::string& outputPath);

/// Convert a T2D module script into a minimal `.fuselevel` scene file (U7).
ConvertResult convertT2DModuleToFuselevel(const std::string& modulePath,
                                          const std::string& outputPath);

/// Cook manifest default worlds to `.fuselevel` when legacy sources are present (U7).
std::vector<ConvertResult> convertManifestWorlds(const ProjectManifest& project,
                                                 const std::string& outputRoot = "");

} // namespace fuse::project
