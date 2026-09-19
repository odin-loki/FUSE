#pragma once

// Ore: third_party/addons/3DAAK/Templates/Full/game/scripts/server/weapon.cs

#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/item_id.hpp>

namespace fuse::adventure {

/// Weapon pickup that grants weapon + ammo items (3DAAK Weapon::onPickup / Ammo::onInventory).
class WeaponPickupInteractable : public IInteractable {
public:
    WeaponPickupInteractable(ItemId weapon, ItemId ammo, u32 ammoAmount = 10);

    ItemId weaponItem() const { return m_weapon; }
    ItemId ammoItem() const { return m_ammo; }
    u32 ammoAmount() const { return m_ammoAmount; }
    bool consumed() const { return m_consumed; }

    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

private:
    ItemId m_weapon;
    ItemId m_ammo;
    u32 m_ammoAmount = 10;
    bool m_consumed = false;
};

} // namespace fuse::adventure
