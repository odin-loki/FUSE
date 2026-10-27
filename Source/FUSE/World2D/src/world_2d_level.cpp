// UNI-U7-WORLD-1: World2D spawn queries over the fuselevel ECS block (FUSE_BUILD_PROJECT / fuse_scene).

#include <fuse/world2d/world_2d.hpp>

#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/transform.hpp>

namespace fuse::world2d {

bool World2D::findSpawnPoint(f32 outPosition[3]) {
    bool found = false;
    ecs::Registry& reg = ownRegistry_();
    reg.each<ecs::SpawnMarker, ecs::Transform>([&](ecs::EntityID, ecs::SpawnMarker& spawn, ecs::Transform& t) {
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

} // namespace fuse::world2d
