#pragma once

#include <fuse/ecs/registry.hpp>

namespace fuse::ecs {

class CameraSystem {
public:
    static void update(Registry& reg);
};

} // namespace fuse::ecs
