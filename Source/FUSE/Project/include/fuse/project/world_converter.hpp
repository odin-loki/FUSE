#pragma once

#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/project/parity_legacy_sources.hpp>
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

struct Ensure3DWorldResult {
    bool ok = false;
    u32 entityCount = 0;
    u32 wiringStubCount = 0;
    LegacySourceOrigin sourceOrigin = LegacySourceOrigin::Missing;
    std::string loadedPath;
    std::string note;
};

/// Ensure `.fuselevel` exists (convert from golden/bundled `.mis` when needed).
[[nodiscard]] Ensure3DWorldResult ensureDefault3DWorldReady(const LoadResult& projectLoad);

struct Ensure2DWorldResult {
    bool ok = false;
    u32 entityCount = 0;
    u32 wiringStubCount = 0;
    LegacySourceOrigin sourceOrigin = LegacySourceOrigin::Missing;
    std::string loadedPath;
    std::string note;
};

/// Ensure default 2D `.fuselevel` exists (convert from golden/bundled `.cs` when needed).
[[nodiscard]] Ensure2DWorldResult ensureDefault2DWorldReady(const LoadResult& projectLoad);

} // namespace fuse::project
