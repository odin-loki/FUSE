#pragma once

// Ore: 3DAAK NPC conversation script VM stub (without TorqueScript)

#include <fuse/adventure/conversation_interactable.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::adventure {

struct ConversationScriptHook {
    std::string npcId;
    std::string branchId;
    std::vector<std::string> lines;
};

/// Dispatches conversation branch hooks into ConversationInteractable (3DAAK script VM stub).
class ConversationScriptVm {
public:
    void registerHook(const ConversationScriptHook& hook);

    bool dispatchBranch(const std::string& npcId,
                        const std::string& branchId,
                        InteractContext& ctx,
                        ConversationInteractable& target);

    u32 dispatchCount() const { return m_dispatchCount; }
    u32 hookCount() const { return static_cast<u32>(m_hooks.size()); }
    const std::string& lastBranchDispatched() const { return m_lastBranchDispatched; }

private:
    std::string hookKey(const std::string& npcId, const std::string& branchId) const;

    std::unordered_map<std::string, ConversationScriptHook> m_hooks;
    u32 m_dispatchCount = 0;
    std::string m_lastBranchDispatched;
};

/// Register built-in Outpost guard conversation branches.
void registerOutpostConversationScriptHooks(ConversationScriptVm& vm);

} // namespace fuse::adventure
