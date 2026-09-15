#include <fuse/adventure/pickup_interactable.hpp>

#include <fuse/adventure/inventory.hpp>

namespace fuse::adventure {

PickupInteractable::PickupInteractable(ItemId item, u32 amount)
    : m_item(std::move(item)), m_amount(amount) {}

InteractResult PickupInteractable::onUse(InteractContext& /*ctx*/, ItemId /*item*/) {
    return InteractResult::Ignored;
}

InteractResult PickupInteractable::onPickup(InteractContext& ctx, ItemId item, u32 amount) {
    if (m_consumed) {
        return InteractResult::Ignored;
    }

    if (ctx.inventory == nullptr) {
        return InteractResult::Failed;
    }

    if (item != m_item && item.isValid()) {
        return InteractResult::Ignored;
    }

    const u32 grantAmount = amount > 0 ? amount : m_amount;
    const u32 granted = ctx.inventory->incInventory(m_item, grantAmount);
    if (granted == 0) {
        return InteractResult::Failed;
    }

    m_consumed = true;
    return InteractResult::PickedUp;
}

} // namespace fuse::adventure
