#pragma once

// Ore: 3DAAK Weapon runtime — fire / ammo consumption stub (without TorqueScript)

#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/item_id.hpp>
#include <fuse/types.hpp>

namespace fuse::adventure {

/// Weapon stat block distilled from 3DAAK `weapon.cs` (damage / range / fire rate ore).
struct WeaponStats {
    f32 damage = 10.f;
    f32 range = 50.f;
    f32 fireRate = 2.f;
};

/// Minimal weapon runtime: consumes ammo from inventory when firing active weapon.
class WeaponRuntime {
public:
    void setAmmoType(ItemId ammoType) { m_ammoType = std::move(ammoType); }
    const ItemId& ammoType() const { return m_ammoType; }

    void setAmmoPerShot(u32 ammoPerShot) { m_ammoPerShot = ammoPerShot; }
    u32 ammoPerShot() const { return m_ammoPerShot; }

    void setStats(const WeaponStats& stats) { m_stats = stats; }
    const WeaponStats& stats() const { return m_stats; }

    bool fire(Inventory& inventory);
    u32 fireCount() const { return m_fireCount; }
    u32 lastAmmoConsumed() const { return m_lastAmmoConsumed; }
    f32 lastDamageDealt() const { return m_lastDamageDealt; }

private:
    ItemId m_ammoType{"energy_cell"};
    WeaponStats m_stats{};
    u32 m_ammoPerShot = 1;
    u32 m_fireCount = 0;
    u32 m_lastAmmoConsumed = 0;
    f32 m_lastDamageDealt = 0.f;
};

} // namespace fuse::adventure
