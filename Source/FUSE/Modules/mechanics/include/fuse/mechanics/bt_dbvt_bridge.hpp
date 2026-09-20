#pragma once

// Ore: Bullet btDbvtBroadphase bridge (real btDbvt when FUSE_BUILD_BULLET; CPU deepen otherwise)

#include <fuse/mechanics/broadphase_proxy_filter.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::mechanics {

struct BtDbvtProxy {
    u32 objectId = 0;
    BroadphaseProxyDesc proxy{};
    float minX = 0.f;
    float minY = 0.f;
    float minZ = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;
    float maxZ = 0.f;
};

/// btDbvt-style dynamic AABB tree bridge.
class BtDbvtBridge {
public:
    void insertProxy(const BtDbvtProxy& proxy);
    bool removeProxy(u32 objectId);
    void clear();

    [[nodiscard]] bool usesBulletDbvt() const;
    [[nodiscard]] u32 proxyCount() const { return static_cast<u32>(m_proxies.size()); }
    [[nodiscard]] u32 overlapQueryCount() const { return m_overlapQueryCount; }
    [[nodiscard]] u32 lastOverlapCount() const { return m_lastOverlapCount; }

    [[nodiscard]] u32 queryOverlaps(BroadphaseProxyFilter filterA, BroadphaseProxyFilter filterB);
    [[nodiscard]] u32 queryAabbOverlaps(float minX, float minY, float minZ, float maxX, float maxY, float maxZ);

private:
    std::vector<BtDbvtProxy> m_proxies;
    u32 m_overlapQueryCount = 0;
    u32 m_lastOverlapCount = 0;
    u32 m_aabbQueryCount = 0;
};

} // namespace fuse::mechanics
