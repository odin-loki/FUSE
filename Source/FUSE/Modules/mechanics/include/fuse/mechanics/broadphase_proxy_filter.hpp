#pragma once

// Ore: Bullet btBroadphaseProxy collision filter groups (CPU stub without Bullet link)

#include <fuse/types.hpp>

namespace fuse::mechanics {

enum class BroadphaseProxyFilter : u8 {
    Default = 0,
    StaticRigid = 1,
    Character = 2,
    Trigger = 4,
};

using BroadphaseProxyGroupMask = u8;

[[nodiscard]] BroadphaseProxyGroupMask broadphaseProxyGroupMask(BroadphaseProxyFilter filter);
[[nodiscard]] bool broadphaseProxyMasksCollide(BroadphaseProxyGroupMask groupA,
                                               BroadphaseProxyGroupMask maskA,
                                               BroadphaseProxyGroupMask groupB,
                                               BroadphaseProxyGroupMask maskB);
[[nodiscard]] bool broadphaseProxyFiltersCollide(BroadphaseProxyFilter a, BroadphaseProxyFilter b);

} // namespace fuse::mechanics
