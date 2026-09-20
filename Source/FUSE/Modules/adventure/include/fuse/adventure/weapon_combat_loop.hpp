#pragma once

// Ore: 3DAAK weapon.cs combat tick loop stub (without TorqueScript)

#include <fuse/adventure/combat_hitscan_stub.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/weapon_runtime.hpp>
#include <fuse/mechanics/health_component.hpp>
#include <fuse/types.hpp>

namespace fuse::adventure {

/// Minimal weapon combat loop: fire-rate gate + damage apply stub.
class WeaponCombatLoop {
public:
    void setActiveWeapon(WeaponRuntime* weapon) { m_weapon = weapon; }
    WeaponRuntime* activeWeapon() const { return m_weapon; }

    void setTarget(fuse::mechanics::HealthComponent* target) { m_target = target; }
    fuse::mechanics::HealthComponent* target() const { return m_target; }

    void setHitscanStub(CombatHitscanStub* hitscan) { m_hitscan = hitscan; }
    CombatHitscanStub* hitscanStub() const { return m_hitscan; }

    void setTargetPosition(f32 x, f32 y, f32 z) {
        m_targetX = x;
        m_targetY = y;
        m_targetZ = z;
    }

    void tick(f32 dt);
    bool tryFire(Inventory& inventory);

    u32 tickCount() const { return m_tickCount; }
    u32 fireAttemptCount() const { return m_fireAttemptCount; }
    u32 damageApplyCount() const { return m_damageApplyCount; }
    u32 hitscanHitCount() const { return m_hitscanHitCount; }
    f32 cooldownRemaining() const { return m_cooldownRemaining; }

private:
    WeaponRuntime* m_weapon = nullptr;
    fuse::mechanics::HealthComponent* m_target = nullptr;
    CombatHitscanStub* m_hitscan = nullptr;
    f32 m_targetX = 0.f;
    f32 m_targetY = 0.f;
    f32 m_targetZ = 0.f;
    f32 m_cooldownRemaining = 0.f;
    u32 m_tickCount = 0;
    u32 m_fireAttemptCount = 0;
    u32 m_damageApplyCount = 0;
    u32 m_hitscanHitCount = 0;
};

} // namespace fuse::adventure
