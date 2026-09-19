#pragma once

// Ore: third_party/addons/3DAAK (NPC conversation / lore scripts in Outpost template)

#include <fuse/adventure/interactable.hpp>

#include <string>
#include <vector>

namespace fuse::adventure {

/// Multi-line NPC conversation (3DAAK conversation scripts without script VM).
class ConversationInteractable : public IInteractable {
public:
    explicit ConversationInteractable(std::vector<std::string> lines);

    const std::vector<std::string>& lines() const { return m_lines; }
    u32 lineIndex() const { return m_lineIndex; }
    u32 converseCount() const { return m_converseCount; }

    const std::string& currentLine() const;
    InteractResult converse(InteractContext& ctx);

    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

private:
    std::vector<std::string> m_lines;
    u32 m_lineIndex = 0;
    u32 m_converseCount = 0;
    std::string m_lastActor;
};

} // namespace fuse::adventure
