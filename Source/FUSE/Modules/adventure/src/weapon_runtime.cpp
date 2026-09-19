#include <fuse/adventure/weapon_runtime.hpp>

namespace fuse::adventure {

bool WeaponRuntime::fire(Inventory& inventory) {
    m_lastAmmoConsumed = 0;
    if (inventory.activeWeapon().name.empty()) {
        return false;
    }
    if (!inventory.hasInventory(m_ammoType) || inventory.getInventory(m_ammoType) < m_ammoPerShot) {
        return false;
    }

    m_lastAmmoConsumed = inventory.decInventory(m_ammoType, m_ammoPerShot);
    if (m_lastAmmoConsumed == 0) {
        return false;
    }

    ++m_fireCount;
    return true;
}

} // namespace fuse::adventure
