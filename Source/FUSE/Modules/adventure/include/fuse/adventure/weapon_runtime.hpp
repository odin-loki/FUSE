#pragma once

// Ore: 3DAAK Weapon runtime — fire / ammo consumption stub (without TorqueScript)

#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/item_id.hpp>
#include <fuse/types.hpp>

namespace fuse::adventure {

/// Minimal weapon runtime: consumes ammo from inventory when firing active weapon.
class WeaponRuntime {
public:
    void setAmmoType(ItemId ammoType) { m_ammoType = std::move(ammoType); }
    const ItemId& ammoType() const { return m_ammoType; }

    void setAmmoPerShot(u32 ammoPerShot) { m_ammoPerShot = ammoPerShot; }
    u32 ammoPerShot() const { return m_ammoPerShot; }

    bool fire(Inventory& inventory);
    u32 fireCount() const { return m_fireCount; }
    u32 lastAmmoConsumed() const { return m_lastAmmoConsumed; }

private:
    ItemId m_ammoType{"energy_cell"};
    u32 m_ammoPerShot = 1;
    u32 m_fireCount = 0;
    u32 m_lastAmmoConsumed = 0;
};

} // namespace fuse::adventure
