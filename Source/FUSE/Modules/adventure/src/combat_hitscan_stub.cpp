#include <fuse/adventure/combat_hitscan_stub.hpp>

#include <cmath>

namespace fuse::adventure {

namespace {

HitscanResult evaluateHitscan(const WeaponStats& stats,
                              f32 spreadDeg,
                              f32 originX,
                              f32 originY,
                              f32 originZ,
                              f32 dirX,
                              f32 dirY,
                              f32 dirZ,
                              f32 targetX,
                              f32 targetY,
                              f32 targetZ) {
    HitscanResult result{};

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
    const f32 spreadRad = spreadDeg * (3.14159265f / 180.f);
    result.spreadYawDeg = spreadDeg;
    result.spreadPitchDeg = spreadDeg * 0.5f;

    if (dot < std::cos(spreadRad)) {
        return result;
    }

    result.hit = true;
    result.damage = stats.damage;
    result.pelletHits = 1;
    return result;
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
    ++m_fireCount;
    return evaluateHitscan(stats, m_spreadDeg, originX, originY, originZ, dirX, dirY, dirZ, targetX, targetY,
                           targetZ);
}

HitscanResult CombatHitscanStub::fireSpreadBurst(const WeaponStats& stats,
                                                 f32 originX,
                                                 f32 originY,
                                                 f32 originZ,
                                                 f32 dirX,
                                                 f32 dirY,
                                                 f32 dirZ,
                                                 f32 targetX,
                                                 f32 targetY,
                                                 f32 targetZ,
                                                 u32 pelletCount) const {
    ++m_spreadBurstCount;
    HitscanResult aggregate{};
    const u32 pellets = pelletCount == 0 ? 1u : pelletCount;
    const f32 pelletSpread = m_spreadDeg / static_cast<f32>(pellets);

    for (u32 pellet = 0; pellet < pellets; ++pellet) {
        const HitscanResult pelletResult =
            evaluateHitscan(stats, pelletSpread, originX, originY, originZ, dirX, dirY, dirZ, targetX, targetY,
                            targetZ);
        if (pelletResult.hit) {
            aggregate.hit = true;
            aggregate.pelletHits += pelletResult.pelletHits;
            aggregate.damage += pelletResult.damage;
            aggregate.distance = pelletResult.distance;
            aggregate.spreadYawDeg = pelletResult.spreadYawDeg;
            aggregate.spreadPitchDeg = pelletResult.spreadPitchDeg;
        }
    }

    if (aggregate.hit) {
        ++m_fireCount;
    }
    return aggregate;
}

HitscanResult CombatHitscanStub::fireHitscanWithPenetration(const WeaponStats& stats,
                                                            f32 originX,
                                                            f32 originY,
                                                            f32 originZ,
                                                            f32 dirX,
                                                            f32 dirY,
                                                            f32 dirZ,
                                                            f32 targetX,
                                                            f32 targetY,
                                                            f32 targetZ,
                                                            u32 maxPenetrationLayers) const {
    ++m_penetrationCount;
    HitscanResult result =
        evaluateHitscan(stats, m_spreadDeg, originX, originY, originZ, dirX, dirY, dirZ, targetX, targetY, targetZ);
    if (!result.hit) {
        return result;
    }

    const u32 layers = maxPenetrationLayers == 0 ? 1u : maxPenetrationLayers;
    result.penetrationLayers = layers;
    result.damage *= static_cast<f32>(layers);
    ++m_fireCount;
    return result;
}

} // namespace fuse::adventure
