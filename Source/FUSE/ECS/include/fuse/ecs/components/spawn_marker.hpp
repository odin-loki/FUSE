#pragma once

#include <fuse/types.hpp>

namespace fuse::ecs {

/// Spawn-point marker distilled from T3D `dataBlock` wires (U7 wave 7).
struct SpawnMarker {
    static constexpr const char* component_name = "SpawnMarker";

    u32 datablock_id = 0;
    bool active = true;
};

} // namespace fuse::ecs
