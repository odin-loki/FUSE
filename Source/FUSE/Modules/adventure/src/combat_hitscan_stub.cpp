#include <fuse/adventure/combat_hitscan_stub.hpp>

#include <cmath>

namespace fuse::adventure {

HitscanResult CombatHitscanStub::fireHitscan(const WeaponStats& stats,
                                             f32 originX,
                                             f32 originY,
                                             f32 originZ,
                                             f32 dirX,
                                             f32 dirY,
                                             f32 dirZ,
                                             f32 targetX,
                                             f32 targetY,
                                             f32 targetZ) const {
    HitscanResult result{};
    ++m_fireCount;

    const f32 dx = targetX - originX;
    const f32 dy = targetY - originY;
    const f32 dz = targetZ - originZ;
    const f32 distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    result.distance = distance;
    if (distance > stats.range || distance <= 0.001f) {
        return result;
    }

    const f32 invDistance = 1.f / distance;
    const f32 aimX = dx * invDistance;
    const f32 aimY = dy * invDistance;
    const f32 aimZ = dz * invDistance;
    const f32 dot = aimX * dirX + aimY * dirY + aimZ * dirZ;
    const f32 spreadRad = m_spreadDeg * (3.14159265f / 180.f);
    result.spreadYawDeg = m_spreadDeg;
    result.spreadPitchDeg = m_spreadDeg * 0.5f;

    if (dot < std::cos(spreadRad)) {
        return result;
    }

    result.hit = true;
    result.damage = stats.damage;
    return result;
}

} // namespace fuse::adventure
