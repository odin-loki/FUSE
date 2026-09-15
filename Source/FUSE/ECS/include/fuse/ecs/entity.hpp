#pragma once

#include <fuse/types.hpp>

namespace fuse::ecs {

/// Handle-based entity identity (index + generation). Stale handles fail `alive()` O(1).
struct EntityID {
    u32 index = 0;
    u32 generation = 0;

    bool operator==(const EntityID& other) const {
        return index == other.index && generation == other.generation;
    }

    bool operator!=(const EntityID& other) const { return !(*this == other); }

    [[nodiscard]] bool valid() const { return generation != 0; }

    static EntityID null() { return {}; }
};

static constexpr u32 kMaxEntities = 1u << 20;

} // namespace fuse::ecs
