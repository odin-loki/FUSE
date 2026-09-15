#include <fuse/adventure/interaction.hpp>

namespace fuse::adventure {

InteractResult InteractionSystem::use(InteractContext& ctx, ItemId item, IInteractable& target) {
    if (ctx.inventory == nullptr || !ctx.inventory->hasInventory(item)) {
        return InteractResult::Failed;
    }

    return target.onUse(ctx, item);
}

InteractResult InteractionSystem::pickup(InteractContext& ctx, ItemId item, u32 amount, IInteractable& target) {
    if (ctx.inventory == nullptr || amount == 0) {
        return InteractResult::Failed;
    }

    return target.onPickup(ctx, item, amount);
}

} // namespace fuse::adventure
