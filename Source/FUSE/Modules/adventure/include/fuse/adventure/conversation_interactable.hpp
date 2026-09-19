#pragma once

// Ore: third_party/addons/3DAAK (NPC conversation / lore scripts in Outpost template)

#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/outpost_loader.hpp>

#include <string>
#include <vector>

namespace fuse::adventure {

/// Multi-line NPC conversation with optional branch selection (3DAAK scripts without VM).
class ConversationInteractable : public IInteractable {
public:
    ConversationInteractable(std::vector<std::string> lines,
                             std::vector<ConversationBranch> branches = {});

    const std::vector<std::string>& lines() const { return m_lines; }
    const std::vector<ConversationBranch>& branches() const { return m_branches; }
    u32 lineIndex() const { return m_lineIndex; }
    u32 converseCount() const { return m_converseCount; }
    const std::string& activeBranchId() const { return m_activeBranchId; }

    const std::string& currentLine() const;
    InteractResult converse(InteractContext& ctx);
    InteractResult chooseBranch(InteractContext& ctx, const std::string& branchId);

    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

private:
    std::vector<std::string> m_lines;
    std::vector<ConversationBranch> m_branches;
    u32 m_lineIndex = 0;
    u32 m_converseCount = 0;
    std::string m_lastActor;
    std::string m_activeBranchId;
};

} // namespace fuse::adventure
