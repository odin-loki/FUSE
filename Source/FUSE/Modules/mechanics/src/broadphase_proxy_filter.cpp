#include <fuse/mechanics/broadphase_proxy_filter.hpp>

namespace fuse::mechanics {

BroadphaseProxyGroupMask broadphaseProxyGroupMask(BroadphaseProxyFilter filter) {
    return static_cast<BroadphaseProxyGroupMask>(filter);
}

BroadphaseProxyDesc makeBroadphaseProxyDesc(BroadphaseProxyFilter filter) {
    const BroadphaseProxyGroupMask allMask = static_cast<BroadphaseProxyGroupMask>(
        static_cast<u8>(BroadphaseProxyFilter::Default) | static_cast<u8>(BroadphaseProxyFilter::StaticRigid) |
        static_cast<u8>(BroadphaseProxyFilter::Character) | static_cast<u8>(BroadphaseProxyFilter::Trigger));

    BroadphaseProxyDesc desc{};
    desc.filter = filter;
    desc.group = broadphaseProxyGroupMask(filter);
    switch (filter) {
    case BroadphaseProxyFilter::Character:
        desc.mask = allMask;
        break;
    case BroadphaseProxyFilter::Trigger:
        desc.mask = static_cast<BroadphaseProxyGroupMask>(BroadphaseProxyFilter::Character);
        break;
    case BroadphaseProxyFilter::StaticRigid:
        desc.mask = static_cast<BroadphaseProxyGroupMask>(BroadphaseProxyFilter::Character);
        break;
    default:
        desc.mask = allMask;
        break;
    }
    return desc;
}

bool broadphaseProxyDescsCollide(const BroadphaseProxyDesc& a, const BroadphaseProxyDesc& b) {
    return broadphaseProxyMasksCollide(a.group, a.mask, b.group, b.mask);
}

bool broadphaseProxyMasksCollide(BroadphaseProxyGroupMask groupA,
                                 BroadphaseProxyGroupMask maskA,
                                 BroadphaseProxyGroupMask groupB,
                                 BroadphaseProxyGroupMask maskB) {
    return (groupA & maskB) != 0 && (groupB & maskA) != 0;
}

bool broadphaseProxyFiltersCollide(BroadphaseProxyFilter a, BroadphaseProxyFilter b) {
    const BroadphaseProxyGroupMask groupA = broadphaseProxyGroupMask(a);
    const BroadphaseProxyGroupMask groupB = broadphaseProxyGroupMask(b);
    const BroadphaseProxyGroupMask allMask = static_cast<BroadphaseProxyGroupMask>(
        static_cast<u8>(BroadphaseProxyFilter::Default) | static_cast<u8>(BroadphaseProxyFilter::StaticRigid) |
        static_cast<u8>(BroadphaseProxyFilter::Character) | static_cast<u8>(BroadphaseProxyFilter::Trigger));

    if (a == BroadphaseProxyFilter::Character && b == BroadphaseProxyFilter::Trigger) {
        return broadphaseProxyMasksCollide(groupA, allMask, groupB, static_cast<BroadphaseProxyGroupMask>(BroadphaseProxyFilter::Character));
    }
    if (b == BroadphaseProxyFilter::Character && a == BroadphaseProxyFilter::Trigger) {
        return broadphaseProxyMasksCollide(groupA, static_cast<BroadphaseProxyGroupMask>(BroadphaseProxyFilter::Character), groupB, allMask);
    }

    if (a == BroadphaseProxyFilter::Default || b == BroadphaseProxyFilter::Default) {
        return true;
    }
    if (a == BroadphaseProxyFilter::Trigger || b == BroadphaseProxyFilter::Trigger) {
        return a == BroadphaseProxyFilter::Character || b == BroadphaseProxyFilter::Character;
    }
    return a == b;
}

} // namespace fuse::mechanics
