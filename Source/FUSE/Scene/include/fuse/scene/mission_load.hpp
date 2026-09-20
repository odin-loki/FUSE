#pragma once

#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>

#include <string>
#include <vector>

namespace fuse::scene {

/// SimObject stub stored in a generation-checked HandleMap (Track A P4).
struct MissionObject {
    std::string className;
    std::string objectName;
    std::string position;
    std::string scale;
    fuse::Handle<MissionObject> handle = fuse::Handle<MissionObject>::invalid();
};

struct MissionLoadResult {
    bool ok = false;
    std::string worldName;
    fuse::HandleMap<MissionObject> objects;
    std::vector<fuse::Handle<MissionObject>> handles;
    std::string error;
};

/// Read a Torque `.mis` from disk, extract SimObject stubs, and insert them
/// into a HandleMap. Returned handles fail `get()` after `remove()`.
[[nodiscard]] MissionLoadResult loadMissionToHandleMap(const std::string& path);

} // namespace fuse::scene
