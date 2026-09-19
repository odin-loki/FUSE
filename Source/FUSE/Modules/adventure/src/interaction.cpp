#include <fuse/adventure/interaction.hpp>

#include <fuse/adventure/examine_interactable.hpp>
#include <fuse/adventure/hud_prompt_interactable.hpp>

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

InteractResult InteractionSystem::examine(InteractContext& ctx, IInteractable& target) {
    if (auto* examinable = dynamic_cast<ExamineInteractable*>(&target)) {
        return examinable->onExamine(ctx);
    }
    return target.onUse(ctx, ItemId(""));
}

std::string InteractionSystem::promptFor(const IInteractable& target) const {
    if (const auto* hud = dynamic_cast<const HudPromptInteractable*>(&target)) {
        return hud->prompt();
    }
    return {};
}

std::string InteractionSystem::showHudPrompt(InteractContext& ctx, IInteractable& target) {
    if (examine(ctx, target) != InteractResult::Examined) {
        return {};
    }
    return promptFor(target);
}

} // namespace fuse::adventure
