#include <fuse/adventure/weapon_mount_animation.hpp>

namespace fuse::adventure {

bool WeaponMountAnimationStub::applyOnGrant(Inventory& inventory, const ItemId& weapon) {
    if (weapon.name.empty()) {
        return false;
    }

    inventory.setActiveWeapon(weapon);
    m_lastWeaponApplied = weapon;
    ++m_applyCount;
    return inventory.activeWeapon().name == weapon.name;
}

} // namespace fuse::adventure
