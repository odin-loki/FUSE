#include <fuse/adventure/door_interactable.hpp>

#include <fuse/adventure/inventory.hpp>

namespace fuse::adventure {

DoorInteractable::DoorInteractable(ItemId keyItem, std::string openMessage)
    : m_keyItem(std::move(keyItem)), m_openMessage(std::move(openMessage)) {}

InteractResult DoorInteractable::onUse(InteractContext& ctx, ItemId item) {
    if (m_open) {
        return InteractResult::Used;
    }

    if (ctx.inventory == nullptr) {
        return InteractResult::Failed;
    }

    if (item != m_keyItem || !ctx.inventory->hasInventory(m_keyItem)) {
        return InteractResult::Ignored;
    }

    ctx.inventory->decInventory(m_keyItem, 1);
    m_open = true;
    return InteractResult::Used;
}

InteractResult DoorInteractable::onPickup(InteractContext& /*ctx*/, ItemId /*item*/, u32 /*amount*/) {
    return InteractResult::Ignored;
}

} // namespace fuse::adventure
