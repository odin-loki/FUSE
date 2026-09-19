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
    std::string requiredItem;
    u32 minInventoryCount = 0;
    std::string grantItem;
    u32 grantAmount = 0;
};

/// Dispatches conversation branch hooks into ConversationInteractable (3DAAK script VM stub).
class ConversationScriptVm {
public:
    void registerHook(const ConversationScriptHook& hook);

    bool dispatchBranch(const std::string& npcId,
                        const std::string& branchId,
                        InteractContext& ctx,
                        ConversationInteractable& target);

    [[nodiscard]] bool canDispatchBranch(const std::string& npcId,
                                         const std::string& branchId,
                                         const InteractContext& ctx) const;

    [[nodiscard]] u32 branchLineCount(const std::string& npcId, const std::string& branchId) const;
    [[nodiscard]] std::string peekBranchLine(const std::string& npcId,
                                             const std::string& branchId,
                                             u32 lineIndex) const;
    [[nodiscard]] u32 dispatchAllLines(const std::string& npcId,
                                       const std::string& branchId,
                                       InteractContext& ctx,
                                       ConversationInteractable& target);

    u32 dispatchCount() const { return m_dispatchCount; }
    u32 lineDispatchCount() const { return m_lineDispatchCount; }
    u32 grantCount() const { return m_grantCount; }
    u32 hookCount() const { return static_cast<u32>(m_hooks.size()); }
    const std::string& lastBranchDispatched() const { return m_lastBranchDispatched; }
    const std::string& lastLineDispatched() const { return m_lastLineDispatched; }
    const std::string& lastGrantedItem() const { return m_lastGrantedItem; }

private:
    std::string hookKey(const std::string& npcId, const std::string& branchId) const;

    std::unordered_map<std::string, ConversationScriptHook> m_hooks;
    u32 m_dispatchCount = 0;
    u32 m_lineDispatchCount = 0;
    u32 m_grantCount = 0;
    std::string m_lastBranchDispatched;
    std::string m_lastLineDispatched;
    std::string m_lastGrantedItem;
};

/// Register built-in Outpost guard conversation branches.
void registerOutpostConversationScriptHooks(ConversationScriptVm& vm);

} // namespace fuse::adventure
