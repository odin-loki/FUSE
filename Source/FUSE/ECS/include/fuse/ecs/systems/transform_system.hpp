#pragma once

#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/math/mat.hpp>
#include <fuse/ecs/registry.hpp>

namespace fuse::ecs {

class TransformSystem {
public:
    static void update(Registry& reg);

private:
    static void update_hierarchy(Registry& reg, EntityID id, const mat4& parent_matrix);
};

} // namespace fuse::ecs
