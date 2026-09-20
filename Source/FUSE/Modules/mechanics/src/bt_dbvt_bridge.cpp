#include <fuse/mechanics/bt_dbvt_bridge.hpp>

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
        if (aabbContains(proxy, minX, minY, minZ, maxX, maxY, maxZ)) {
            ++overlaps;
        }
    }
    return overlaps;
}

} // namespace fuse::mechanics
