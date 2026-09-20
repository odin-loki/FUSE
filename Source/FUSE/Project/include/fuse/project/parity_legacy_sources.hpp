#pragma once

#include <fuse/project/loader.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::project {

enum class LegacySourceOrigin : u8 {
    Missing = 0,
    Bundled,
    GoldenSubmodule,
};

/// Local-only submodule directory probe (no `git submodule init` / network fetch).
enum class SubmodulePathStatus : u8 {
    Absent = 0,
    Uninitialized,
    Present,
};

struct LegacySourceResolution {
    std::string path;
    LegacySourceOrigin origin = LegacySourceOrigin::Missing;
    SubmodulePathStatus goldenSubmoduleStatus = SubmodulePathStatus::Absent;
    std::string note;
};

/// Walk upward from `startPath` to locate the FUSE repository root.
[[nodiscard]] std::string findRepositoryRoot(const std::string& startPath);

/// Classify a submodule checkout directory without network access.
[[nodiscard]] SubmodulePathStatus classifySubmoduleDirectory(const std::string& directoryPath);

/// Resolve a `.mis` or `.cs` source for a parity demo world, preferring golden submodule paths.
[[nodiscard]] LegacySourceResolution resolveParityLegacySource(const ProjectManifest& manifest,
                                                                 const std::string& fuselevelPath,
                                                                 const char* extension);

} // namespace fuse::project
