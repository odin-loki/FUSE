#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::cook {

enum class FuselevelCookStatus : u8 {
    Ok = 0,
    IoError,
    UnsupportedSource,
};

struct FuselevelCookResult {
    FuselevelCookStatus status = FuselevelCookStatus::IoError;
    u32 entityCount = 0;
    u32 hierarchyLinks = 0;
    std::string note;
};

/// Stub world cooker — delegates to `fuse::project::convertT3DMissionToFuselevel` / module path (U7).
FuselevelCookResult cookFuselevelFromMis(const std::string& missionPath, const std::string& outputPath);
FuselevelCookResult cookFuselevelFromModule(const std::string& modulePath, const std::string& outputPath);

} // namespace fuse::cook
