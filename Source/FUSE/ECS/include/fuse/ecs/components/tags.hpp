#pragma once

namespace fuse::ecs {

struct TagStatic {
    static constexpr const char* component_name = "TagStatic";
};

struct TagPlayer {
    static constexpr const char* component_name = "TagPlayer";
};

struct TagDestroy {
    static constexpr const char* component_name = "TagDestroy";
};

} // namespace fuse::ecs
