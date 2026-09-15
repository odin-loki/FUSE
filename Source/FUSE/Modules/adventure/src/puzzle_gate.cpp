#include <fuse/adventure/puzzle_gate.hpp>

#include <fuse/adventure/inventory.hpp>

namespace fuse::adventure {

PuzzleGate::PuzzleGate(ItemId requiredItem) : m_requiredItem(std::move(requiredItem)) {}

InteractResult PuzzleGate::onUse(InteractContext& ctx, ItemId item) {
    if (m_unlocked) {
        return InteractResult::Used;
    }

    if (ctx.inventory == nullptr) {
        return InteractResult::Failed;
    }

    if (!ctx.inventory->hasInventory(m_requiredItem)) {
        return InteractResult::Failed;
    }

    if (item == m_requiredItem) {
        m_unlocked = true;
        ctx.inventory->decInventory(m_requiredItem, 1);
        return InteractResult::Used;
    }

    return InteractResult::Ignored;
}

InteractResult PuzzleGate::onPickup(InteractContext& /*ctx*/, ItemId /*item*/, u32 /*amount*/) {
    return InteractResult::Ignored;
}

} // namespace fuse::adventure
