#pragma once

#include <fuse/dimension/world_handle.hpp>
#include <fuse/project/manifest.hpp>

#include <string>
#include <vector>

namespace fuse::project {

enum class ImportSourceKind : u8 {
    T3DMission,
    T2DModule,
};

struct ImportRecord {
    ImportSourceKind kind = ImportSourceKind::T3DMission;
    std::string sourcePath;
    std::string worldName;
    dimension::WorldHandle worldHandle = dimension::WorldHandle::invalid();
    bool ok = false;
    std::string note;
};

struct ImportDryRunResult {
    bool ok = false;
    std::vector<ImportRecord> worlds;
    std::string summary;
};

ImportDryRunResult importDryRun(const ProjectManifest& project, const std::string& sourcePath);
ImportRecord importT3DMission(const std::string& missionPath, u32 worldIndex);
ImportRecord importT2DModule(const std::string& modulePath, u32 worldIndex);

const char* importSourceKindName(ImportSourceKind kind);

} // namespace fuse::project
