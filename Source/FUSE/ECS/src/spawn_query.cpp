#include <fuse/ecs/spawn_query.hpp>

#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>

namespace fuse::ecs {

bool findFirstActiveSpawnWorldPosition(Registry& registry, f32 outPosition[3]) {
    bool found = false;
    registry.each<SpawnMarker, Transform>([&](EntityID, const SpawnMarker& spawn, const Transform& t) {
        if (found || !spawn.active) {
            return;
        }
        outPosition[0] = t.local_to_world.data[12];
        outPosition[1] = t.local_to_world.data[13];
        outPosition[2] = t.local_to_world.data[14];
        found = true;
    });
    return found;
}

} // namespace fuse::ecs
