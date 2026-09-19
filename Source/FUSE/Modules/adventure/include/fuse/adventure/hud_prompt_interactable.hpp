#pragma once

// Ore: third_party/addons/3DAAK (2D HUD interaction prompt / use hint scripts)

#include <fuse/adventure/interactable.hpp>

#include <string>

namespace fuse::adventure {

/// World object that exposes a 2D HUD prompt string (3DAAK interface hint pattern).
class HudPromptInteractable : public IInteractable {
public:
    explicit HudPromptInteractable(std::string prompt);

    const std::string& prompt() const { return m_prompt; }
    u32 promptShownCount() const { return m_promptShownCount; }

    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

    InteractResult showPrompt(InteractContext& ctx);

private:
    std::string m_prompt;
    u32 m_promptShownCount = 0;
};

} // namespace fuse::adventure
