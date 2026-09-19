#include <fuse/adventure/conversation_script_vm.hpp>

namespace fuse::adventure {

std::string ConversationScriptVm::hookKey(const std::string& npcId, const std::string& branchId) const {
    return npcId + ":" + branchId;
}

void ConversationScriptVm::registerHook(const ConversationScriptHook& hook) {
    m_hooks[hookKey(hook.npcId, hook.branchId)] = hook;
}

bool ConversationScriptVm::dispatchBranch(const std::string& npcId,
                                           const std::string& branchId,
                                           InteractContext& ctx,
                                           ConversationInteractable& target) {
    const auto it = m_hooks.find(hookKey(npcId, branchId));
    if (it == m_hooks.end()) {
        return false;
    }

    ++m_dispatchCount;
    m_lastBranchDispatched = branchId;
    if (!it->second.lines.empty()) {
        m_lastLineDispatched = it->second.lines.front();
    } else {
        m_lastLineDispatched.clear();
    }
    return target.chooseBranch(ctx, branchId) == InteractResult::Examined;
}

void registerOutpostConversationScriptHooks(ConversationScriptVm& vm) {
    vm.registerHook({"outpost_guard", "polite", {"Thank you, traveler. Proceed with caution."}});
    vm.registerHook({"outpost_guard", "aggressive", {"Stand down or be fired upon."}});
}

} // namespace fuse::adventure
