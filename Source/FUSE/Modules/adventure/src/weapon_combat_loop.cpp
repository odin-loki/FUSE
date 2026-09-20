#include <fuse/adventure/weapon_combat_loop.hpp>

#include <algorithm>

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

    if (m_target != nullptr && m_target->isAlive()) {
        const s32 damage = static_cast<s32>(m_weapon->lastDamageDealt());
        if (damage > 0) {
            m_target->applyDamage(damage);
            ++m_damageApplyCount;
        }
    }

    return true;
}

} // namespace fuse::adventure
