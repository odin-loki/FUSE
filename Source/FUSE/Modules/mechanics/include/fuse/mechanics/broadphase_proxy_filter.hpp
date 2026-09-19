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

/// btBroadphaseProxy-style group/mask pair (Bullet proxy deepen without linking Bullet).
struct BroadphaseProxyDesc {
    BroadphaseProxyFilter filter = BroadphaseProxyFilter::Default;
    BroadphaseProxyGroupMask group = 0;
    BroadphaseProxyGroupMask mask = 0;
};

[[nodiscard]] BroadphaseProxyGroupMask broadphaseProxyGroupMask(BroadphaseProxyFilter filter);
[[nodiscard]] BroadphaseProxyDesc makeBroadphaseProxyDesc(BroadphaseProxyFilter filter);
[[nodiscard]] bool broadphaseProxyMasksCollide(BroadphaseProxyGroupMask groupA,
                                               BroadphaseProxyGroupMask maskA,
                                               BroadphaseProxyGroupMask groupB,
                                               BroadphaseProxyGroupMask maskB);
[[nodiscard]] bool broadphaseProxyDescsCollide(const BroadphaseProxyDesc& a, const BroadphaseProxyDesc& b);
[[nodiscard]] bool broadphaseProxyFiltersCollide(BroadphaseProxyFilter a, BroadphaseProxyFilter b);

} // namespace fuse::mechanics
