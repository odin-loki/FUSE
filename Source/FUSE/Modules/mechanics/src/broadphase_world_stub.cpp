#include <fuse/mechanics/broadphase_proxy_filter.hpp>
#include <fuse/mechanics/broadphase_world_stub.hpp>

#include <cmath>

namespace fuse::mechanics {

namespace {

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
#endif

} // namespace

void BroadphaseWorldStub::addBody(const BroadphaseWorldBody& body) {
    m_bodies.push_back(body);
}

bool BroadphaseWorldStub::removeBody(u32 objectId) {
    for (auto it = m_bodies.begin(); it != m_bodies.end(); ++it) {
        if (it->objectId == objectId) {
            m_bodies.erase(it);
            return true;
        }
    }
    return false;
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

#if defined(FUSE_HAS_BULLET) && FUSE_HAS_BULLET
    const BroadphaseProxyDesc proxyA = bulletProxyForFilter(filterA);
    const BroadphaseProxyDesc proxyB = bulletProxyForFilter(filterB);
    if (!broadphaseProxyDescsCollide(proxyA, proxyB)) {
        return 0;
    }
#endif

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

u32 BroadphaseWorldStub::queryAabbOverlaps(float minX, float minY, float minZ, float maxX, float maxY,
                                           float maxZ) {
    ++m_aabbQueryCount;
    m_lastOverlapCount = 0;

    for (const BroadphaseWorldBody& body : m_bodies) {
        if (body.x >= minX && body.x <= maxX && body.y >= minY && body.y <= maxY && body.z >= minZ &&
            body.z <= maxZ) {
            ++m_lastOverlapCount;
        }
    }

    return m_lastOverlapCount;
}

u32 BroadphaseWorldStub::queryRaycastStub(float originX, float originY, float originZ, float dirX,
                                          float dirY, float dirZ, float maxDistance) {
    ++m_raycastQueryCount;
    m_lastOverlapCount = 0;

    const float dirLen = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
    if (dirLen <= 0.0001f || maxDistance <= 0.f) {
        return 0;
    }

    const float invLen = 1.f / dirLen;
    const float ndx = dirX * invLen;
    const float ndy = dirY * invLen;
    const float ndz = dirZ * invLen;

    for (const BroadphaseWorldBody& body : m_bodies) {
        const float toX = body.x - originX;
        const float toY = body.y - originY;
        const float toZ = body.z - originZ;
        const float projection = toX * ndx + toY * ndy + toZ * ndz;
        if (projection < 0.f || projection > maxDistance) {
            continue;
        }

        const float closestX = originX + ndx * projection;
        const float closestY = originY + ndy * projection;
        const float closestZ = originZ + ndz * projection;
        const float dx = body.x - closestX;
        const float dy = body.y - closestY;
        const float dz = body.z - closestZ;
        if ((dx * dx + dy * dy + dz * dz) <= 1.f) {
            ++m_lastOverlapCount;
        }
    }

    return m_lastOverlapCount;
}

} // namespace fuse::mechanics
