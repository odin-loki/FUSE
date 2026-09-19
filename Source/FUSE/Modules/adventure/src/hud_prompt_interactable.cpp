#include <fuse/adventure/hud_prompt_interactable.hpp>

namespace fuse::adventure {

HudPromptInteractable::HudPromptInteractable(std::string prompt) : m_prompt(std::move(prompt)) {}

InteractResult HudPromptInteractable::onUse(InteractContext& ctx, ItemId item) {
    (void)item;
    return showPrompt(ctx);
}

InteractResult HudPromptInteractable::onPickup(InteractContext& /*ctx*/, ItemId /*item*/, u32 /*amount*/) {
    return InteractResult::Ignored;
}

InteractResult HudPromptInteractable::showPrompt(InteractContext& ctx) {
    (void)ctx;
    ++m_promptShownCount;
    return InteractResult::Examined;
}

} // namespace fuse::adventure
