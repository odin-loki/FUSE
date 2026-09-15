#pragma once

#include <fuse/adventure/interactable.hpp>

namespace fuse::adventure {

/// Simple gated interaction — unlocked when a required item is present in inventory.
///
/// Mirrors adventure-template patterns where doors / switches require keys or items.
class PuzzleGate : public IInteractable {
public:
    explicit PuzzleGate(ItemId requiredItem);

    bool isUnlocked() const { return m_unlocked; }

    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

private:
    ItemId m_requiredItem;
    bool m_unlocked = false;
};

} // namespace fuse::adventure
