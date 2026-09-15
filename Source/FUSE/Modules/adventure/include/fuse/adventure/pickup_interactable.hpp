#pragma once

#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/item_id.hpp>

namespace fuse::adventure {

/// World pickup that grants an item into the actor inventory (3DAAK `ItemData::onPickup`).
class PickupInteractable : public IInteractable {
public:
    explicit PickupInteractable(ItemId item, u32 amount = 1);

    ItemId item() const { return m_item; }
    u32 amount() const { return m_amount; }
    bool consumed() const { return m_consumed; }

    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

private:
    ItemId m_item;
    u32 m_amount = 1;
    bool m_consumed = false;
};

} // namespace fuse::adventure
