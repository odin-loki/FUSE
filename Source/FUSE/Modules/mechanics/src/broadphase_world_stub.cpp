#include <fuse/mechanics/broadphase_world_stub.hpp>

namespace fuse::mechanics {

void BroadphaseWorldStub::addBody(const BroadphaseWorldBody& body) {
    m_bodies.push_back(body);
}

void BroadphaseWorldStub::setBodyPosition(u32 objectId, float x, float y, float z) {
    for (BroadphaseWorldBody& body : m_bodies) {
        if (body.objectId == objectId) {
            body.x = x;
            body.y = y;
            body.z = z;
            return;
        }
    }
}

void BroadphaseWorldStub::clear() {
    m_bodies.clear();
    m_lastOverlapCount = 0;
}

u32 BroadphaseWorldStub::queryOverlaps(BroadphaseProxyFilter filterA, BroadphaseProxyFilter filterB) {
    ++m_overlapQueryCount;
    m_lastOverlapCount = 0;

    for (usize i = 0; i < m_bodies.size(); ++i) {
        for (usize j = i + 1; j < m_bodies.size(); ++j) {
            const BroadphaseWorldBody& a = m_bodies[i];
            const BroadphaseWorldBody& b = m_bodies[j];
            if (!broadphaseProxyFiltersCollide(a.proxy.filter, b.proxy.filter) &&
                !broadphaseProxyFiltersCollide(filterA, filterB)) {
                continue;
            }

            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            if ((dx * dx + dy * dy + dz * dz) <= 4.f) {
                ++m_lastOverlapCount;
            }
        }
    }

    return m_lastOverlapCount;
}

} // namespace fuse::mechanics
