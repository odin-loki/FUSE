#include <fuse/adventure/combat_hitscan_stub.hpp>

#include <cmath>

namespace fuse::adventure {

namespace {

f32 clampUnit(f32 value) {
    if (value < 0.f) {
        return 0.f;
    }
    if (value > 1.f) {
        return 1.f;
    }
    return value;
}

/// Linear falloff: full `stats.damage` at the muzzle, `damage * falloffMinScale` at `range`.
f32 damageWithFalloff(const WeaponStats& stats, f32 distance) {
    const f32 minScale = clampUnit(stats.falloffMinScale);
    if (minScale >= 1.f || stats.range <= 0.001f) {
        return stats.damage;
    }

    f32 t = 0.f;
    if (distance >= stats.range) {
        t = 1.f;
    } else if (distance > 0.f) {
        t = distance / stats.range;
    }
    return stats.damage * (1.f + (minScale - 1.f) * t);
}

} // namespace

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
    result.damage = damageWithFalloff(stats, distance);
    return result;
}

} // namespace fuse::adventure
