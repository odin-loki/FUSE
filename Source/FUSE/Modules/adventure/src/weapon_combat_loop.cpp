#include <fuse/adventure/weapon_combat_loop.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::adventure {

void WeaponCombatLoop::tick(f32 dt) {
    ++m_tickCount;
    if (m_cooldownRemaining > 0.f) {
        m_cooldownRemaining = std::max(0.f, m_cooldownRemaining - dt);
    }
}

bool WeaponCombatLoop::tryFire(Inventory& inventory) {
    ++m_fireAttemptCount;
    if (m_weapon == nullptr || m_cooldownRemaining > 0.f) {
        return false;
    }

    if (!m_weapon->fire(inventory)) {
        return false;
    }

    const f32 fireRate = m_weapon->stats().fireRate;
    m_cooldownRemaining = fireRate > 0.f ? (1.f / fireRate) : 0.f;

    bool applyDamage = true;
    if (m_hitscan != nullptr) {
        const f32 dx = m_targetX;
        const f32 dy = m_targetY;
        const f32 dz = m_targetZ;
        const f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
        const f32 dirX = len > 0.001f ? dx / len : 1.f;
        const f32 dirY = len > 0.001f ? dy / len : 0.f;
        const f32 dirZ = len > 0.001f ? dz / len : 0.f;
        const HitscanResult hit = m_hitscan->fireHitscan(m_weapon->stats(), 0.f, 0.f, 0.f, dirX, dirY, dirZ,
                                                         m_targetX, m_targetY, m_targetZ);
        applyDamage = hit.hit;
        if (hit.hit) {
            ++m_hitscanHitCount;
        }
    }

    if (applyDamage && m_target != nullptr && m_target->isAlive()) {
        const s32 damage = static_cast<s32>(m_weapon->lastDamageDealt());
        if (damage > 0) {
            m_target->applyDamage(damage);
            ++m_damageApplyCount;
        }
    }

    return true;
}

} // namespace fuse::adventure
