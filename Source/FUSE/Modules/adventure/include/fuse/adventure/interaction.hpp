#pragma once

#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/inventory.hpp>

namespace fuse::adventure {

/// Dispatches use / pickup commands (maps to 3DAAK `ShapeBase::use` / `pickup`).
class InteractionSystem {
public:
    /// Use an item from inventory when count > 0 (3DAAK `ShapeBase::use`).
    InteractResult use(InteractContext& ctx, ItemId item, IInteractable& target);

    /// Pick up an item through the target's onPickup hook (3DAAK `ShapeBase::pickup`).
    InteractResult pickup(InteractContext& ctx, ItemId item, u32 amount, IInteractable& target);
};

} // namespace fuse::adventure
