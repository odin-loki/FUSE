#pragma once

namespace fuse::ecs {

struct TagStatic {
    static constexpr const char* component_name = "TagStatic";
};

struct TagPlayer {
    static constexpr const char* component_name = "TagPlayer";
};

/// Physics body driven by game code (Transform or PhysicsManager::setKinematicTarget); it pushes
/// dynamic bodies but is never pushed back.
struct TagKinematic {
    static constexpr const char* component_name = "TagKinematic";
};

struct TagDestroy {
    static constexpr const char* component_name = "TagDestroy";
};

} // namespace fuse::ecs
