#include <fuse/mechanics/bt_dbvt_bridge.hpp>

#include <fuse/mechanics/broadphase_proxy_filter.hpp>

namespace fuse::mechanics {

namespace {

bool aabbOverlaps(const BtDbvtProxy& a, const BtDbvtProxy& b) {
    return a.maxX >= b.minX && a.minX <= b.maxX && a.maxY >= b.minY && a.minY <= b.maxY &&
           a.maxZ >= b.minZ && a.minZ <= b.maxZ;
}

bool aabbContains(const BtDbvtProxy& proxy, float minX, float minY, float minZ, float maxX, float maxY,
                  float maxZ) {
    return proxy.minX <= maxX && proxy.maxX >= minX && proxy.minY <= maxY && proxy.maxY >= minY &&
           proxy.minZ <= maxZ && proxy.maxZ >= minZ;
}

#if defined(FUSE_HAS_BULLET) && FUSE_HAS_BULLET
constexpr BroadphaseProxyGroupMask kBulletStaticGroup = 1u;
constexpr BroadphaseProxyGroupMask kBulletCharacterGroup = 2u;
constexpr BroadphaseProxyGroupMask kBulletTriggerGroup = 4u;

BroadphaseProxyDesc bulletProxyForFilter(BroadphaseProxyFilter filter) {
    BroadphaseProxyDesc desc = makeBroadphaseProxyDesc(filter);
    switch (filter) {
    case BroadphaseProxyFilter::StaticRigid:
        desc.group = kBulletStaticGroup;
        desc.mask = kBulletCharacterGroup | kBulletTriggerGroup;
        break;
    case BroadphaseProxyFilter::Character:
        desc.group = kBulletCharacterGroup;
        desc.mask = kBulletStaticGroup | kBulletTriggerGroup;
        break;
    case BroadphaseProxyFilter::Trigger:
        desc.group = kBulletTriggerGroup;
        desc.mask = kBulletCharacterGroup;
        break;
    default:
        break;
    }
    return desc;
}

bool bulletProxiesCollide(const BtDbvtProxy& proxyA, const BtDbvtProxy& proxyB) {
    const BroadphaseProxyDesc descA = bulletProxyForFilter(proxyA.proxy.filter);
    const BroadphaseProxyDesc descB = bulletProxyForFilter(proxyB.proxy.filter);
    return broadphaseProxyDescsCollide(descA, descB);
}
#endif

} // namespace

bool BtDbvtBridge::usesBulletDbvt() const {
#if defined(FUSE_HAS_BULLET) && FUSE_HAS_BULLET
    return true;
#else
    return false;
#endif
}

void BtDbvtBridge::insertProxy(const BtDbvtProxy& proxy) {
    for (BtDbvtProxy& existing : m_proxies) {
        if (existing.objectId == proxy.objectId) {
            existing = proxy;
            return;
        }
    }
    m_proxies.push_back(proxy);
}

bool BtDbvtBridge::removeProxy(u32 objectId) {
    for (auto it = m_proxies.begin(); it != m_proxies.end(); ++it) {
        if (it->objectId == objectId) {
            m_proxies.erase(it);
            return true;
        }
    }
    return false;
}

void BtDbvtBridge::clear() {
    m_proxies.clear();
}

u32 BtDbvtBridge::queryOverlaps(BroadphaseProxyFilter filterA, BroadphaseProxyFilter filterB) {
    ++m_overlapQueryCount;
    u32 overlaps = 0;

#if defined(FUSE_HAS_BULLET) && FUSE_HAS_BULLET
    const BroadphaseProxyDesc proxyDescA = bulletProxyForFilter(filterA);
    const BroadphaseProxyDesc proxyDescB = bulletProxyForFilter(filterB);
    if (!broadphaseProxyDescsCollide(proxyDescA, proxyDescB)) {
        m_lastOverlapCount = 0;
        return 0;
    }
#endif

    for (const BtDbvtProxy& proxyA : m_proxies) {
        if (proxyA.proxy.filter != filterA) {
            continue;
        }
        for (const BtDbvtProxy& proxyB : m_proxies) {
            if (proxyB.proxy.filter != filterB || proxyA.objectId == proxyB.objectId) {
                continue;
            }
            if (!broadphaseProxyFiltersCollide(proxyA.proxy.filter, proxyB.proxy.filter)) {
                continue;
            }
#if defined(FUSE_HAS_BULLET) && FUSE_HAS_BULLET
            if (!bulletProxiesCollide(proxyA, proxyB)) {
                continue;
            }
#endif
            if (aabbOverlaps(proxyA, proxyB)) {
                ++overlaps;
            }
        }
    }
    m_lastOverlapCount = overlaps;
    return overlaps;
}

u32 BtDbvtBridge::queryAabbOverlaps(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
    ++m_aabbQueryCount;
    u32 overlaps = 0;
    for (const BtDbvtProxy& proxy : m_proxies) {
#if defined(FUSE_HAS_BULLET) && FUSE_HAS_BULLET
        const BroadphaseProxyDesc desc = bulletProxyForFilter(proxy.proxy.filter);
        if (desc.group == 0u && desc.mask == 0u) {
            continue;
        }
#endif
        if (aabbContains(proxy, minX, minY, minZ, maxX, maxY, maxZ)) {
            ++overlaps;
        }
    }
    return overlaps;
}

} // namespace fuse::mechanics
