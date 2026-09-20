#pragma once

// Ore: 3DAAK weapon hitscan / spread stubs (without TorqueScript)

#include <fuse/adventure/weapon_runtime.hpp>
#include <fuse/types.hpp>

namespace fuse::adventure {

struct HitscanResult {
    bool hit = false;
    f32 damage = 0.f;
    f32 distance = 0.f;
    f32 spreadYawDeg = 0.f;
    f32 spreadPitchDeg = 0.f;
};

class CombatHitscanStub {
public:
    void setSpreadDeg(f32 spreadDeg) { m_spreadDeg = spreadDeg; }
    f32 spreadDeg() const { return m_spreadDeg; }

    [[nodiscard]] HitscanResult fireHitscan(const WeaponStats& stats,
                                            f32 originX,
                                            f32 originY,
                                            f32 originZ,
                                            f32 dirX,
                                            f32 dirY,
                                            f32 dirZ,
                                            f32 targetX,
                                            f32 targetY,
                                            f32 targetZ) const;

    u32 fireCount() const { return m_fireCount; }

private:
    f32 m_spreadDeg = 2.f;
    mutable u32 m_fireCount = 0;
};

} // namespace fuse::adventure
