#pragma once

#include <fuse/types.hpp>

namespace fuse::ecs {

class Registry;

/// First active `SpawnMarker` with a `Transform`: world-space position from `local_to_world` translation.
/// Returns false when no spawn exists.
bool findFirstActiveSpawnWorldPosition(Registry& registry, f32 outPosition[3]);

} // namespace fuse::ecs
