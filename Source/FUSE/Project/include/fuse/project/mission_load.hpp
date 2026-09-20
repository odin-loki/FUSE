#pragma once

#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>

#include <string>
#include <vector>

namespace fuse::project {

struct MissionObject {
    std::string className;
    std::string name;
    std::string position;
    std::string scale;
    std::string datablockRef;
    fuse::Handle<MissionObject> parent = fuse::Handle<MissionObject>::invalid();
};

struct MissionLoadResult {
    bool ok = false;
    std::string worldName;
    fuse::HandleMap<MissionObject> objects;
    std::vector<fuse::Handle<MissionObject>> handles;
    std::vector<fuse::Handle<MissionObject>> roots;
    std::string error;
};

/// Load a Torque `.mis` from disk into a generation-checked HandleMap (Track A P4).
MissionLoadResult loadMissionFile(const std::string& path);

} // namespace fuse::project
