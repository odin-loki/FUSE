// UNI-U7-WORLD-1: World2D spawn queries over the fuselevel ECS block (FUSE_BUILD_PROJECT / fuse_scene).

#include <fuse/world2d/world_2d.hpp>

#include <fuse/ecs/spawn_query.hpp>

namespace fuse::world2d {

bool World2D::findSpawnPoint(f32 outPosition[3]) {
    return ecs::findFirstActiveSpawnWorldPosition(ownRegistry_(), outPosition);
}

} // namespace fuse::world2d
