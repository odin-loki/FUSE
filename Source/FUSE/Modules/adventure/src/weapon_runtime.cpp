#include <fuse/adventure/weapon_runtime.hpp>

namespace fuse::adventure {

bool WeaponRuntime::fire(Inventory& inventory) {
    m_lastAmmoConsumed = 0;
    m_lastDamageDealt = 0.f;
    if (m_reloading) {
        return false;
    }
    if (inventory.activeWeapon().name.empty()) {
        return false;
    }
    if (m_magazineAmmo < m_ammoPerShot) {
        return false;
    }
    if (!inventory.hasInventory(m_ammoType) || inventory.getInventory(m_ammoType) < m_ammoPerShot) {
        return false;
    }

    m_lastAmmoConsumed = inventory.decInventory(m_ammoType, m_ammoPerShot);
    if (m_lastAmmoConsumed == 0) {
        return false;
    }

    m_magazineAmmo -= m_ammoPerShot;
    m_lastDamageDealt = m_stats.damage;
    m_lastSpreadDeg = m_stats.spreadDeg;
    ++m_fireCount;
    return true;
}

bool WeaponRuntime::reload(Inventory& inventory) {
    if (m_reloading || inventory.activeWeapon().name.empty()) {
        return false;
    }
    if (m_magazineAmmo >= m_stats.magazineSize) {
        return false;
    }
    if (!inventory.hasInventory(m_ammoType)) {
        return false;
    }

    m_reloading = true;
    m_reloadElapsedMs = 0;
    ++m_reloadCount;
    return true;
}

bool WeaponRuntime::advanceReload(u32 deltaMs) {
    if (!m_reloading) {
        return false;
    }

    m_reloadElapsedMs += deltaMs;
    if (m_reloadElapsedMs < m_stats.reloadMs) {
        return false;
    }

    m_magazineAmmo = m_stats.magazineSize;
    m_reloading = false;
    m_reloadElapsedMs = 0;
    return true;
}

} // namespace fuse::adventure
