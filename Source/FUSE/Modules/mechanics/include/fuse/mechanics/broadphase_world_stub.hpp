#pragma once

// Ore: Bullet btCollisionWorld proxy registry (CPU stub without Bullet link)

#include <fuse/mechanics/broadphase_proxy_filter.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::mechanics {

struct BroadphaseWorldBody {
    u32 objectId = 0;
    BroadphaseProxyDesc proxy{};
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

/// btCollisionWorld-style proxy registry deepen without linking Bullet.
class BroadphaseWorldStub {
public:
    void addBody(const BroadphaseWorldBody& body);
    void setBodyPosition(u32 objectId, float x, float y, float z);
    void clear();

    [[nodiscard]] u32 bodyCount() const { return static_cast<u32>(m_bodies.size()); }
    [[nodiscard]] u32 overlapQueryCount() const { return m_overlapQueryCount; }
    [[nodiscard]] u32 lastOverlapCount() const { return m_lastOverlapCount; }

    [[nodiscard]] u32 queryOverlaps(BroadphaseProxyFilter filterA, BroadphaseProxyFilter filterB);

private:
    std::vector<BroadphaseWorldBody> m_bodies;
    u32 m_overlapQueryCount = 0;
    u32 m_lastOverlapCount = 0;
};

} // namespace fuse::mechanics
