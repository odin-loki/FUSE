#include <fuse/adventure/weapon_pickup_interactable.hpp>

#include <fuse/adventure/inventory.hpp>

namespace fuse::adventure {

WeaponPickupInteractable::WeaponPickupInteractable(ItemId weapon, ItemId ammo, u32 ammoAmount)
    : m_weapon(std::move(weapon)), m_ammo(std::move(ammo)), m_ammoAmount(ammoAmount) {}

InteractResult WeaponPickupInteractable::onUse(InteractContext& /*ctx*/, ItemId /*item*/) {
    return InteractResult::Ignored;
}

InteractResult WeaponPickupInteractable::onPickup(InteractContext& ctx, ItemId item, u32 amount) {
    if (m_consumed || ctx.inventory == nullptr) {
        return InteractResult::Ignored;
    }
    if (item != m_weapon || amount == 0) {
        return InteractResult::Ignored;
    }

    ctx.inventory->incInventory(m_weapon, 1);
    ctx.inventory->incInventory(m_ammo, m_ammoAmount);
    m_consumed = true;
    return InteractResult::PickedUp;
}

} // namespace fuse::adventure
