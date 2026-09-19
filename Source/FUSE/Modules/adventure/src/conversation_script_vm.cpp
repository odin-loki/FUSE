#include <fuse/adventure/conversation_script_vm.hpp>

#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/item_id.hpp>

namespace fuse::adventure {

std::string ConversationScriptVm::hookKey(const std::string& npcId, const std::string& branchId) const {
    return npcId + ":" + branchId;
}

void ConversationScriptVm::registerHook(const ConversationScriptHook& hook) {
    m_hooks[hookKey(hook.npcId, hook.branchId)] = hook;
}

bool ConversationScriptVm::canDispatchBranch(const std::string& npcId,
                                              const std::string& branchId,
                                              const InteractContext& ctx) const {
    const auto it = m_hooks.find(hookKey(npcId, branchId));
    if (it == m_hooks.end()) {
        return false;
    }

    if (!it->second.requiredItem.empty()) {
        if (ctx.inventory == nullptr) {
            return false;
        }
        return ctx.inventory->getInventory(ItemId(it->second.requiredItem)) >= it->second.minInventoryCount;
    }

    return true;
}

bool ConversationScriptVm::dispatchBranch(const std::string& npcId,
                                           const std::string& branchId,
                                           InteractContext& ctx,
                                           ConversationInteractable& target) {
    const auto it = m_hooks.find(hookKey(npcId, branchId));
    if (it == m_hooks.end()) {
        return false;
    }

    if (!canDispatchBranch(npcId, branchId, ctx)) {
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
    ConversationScriptHook polite{};
    polite.npcId = "outpost_guard";
    polite.branchId = "polite";
    polite.lines = {"Thank you, traveler. Proceed with caution."};

    ConversationScriptHook aggressive{};
    aggressive.npcId = "outpost_guard";
    aggressive.branchId = "aggressive";
    aggressive.lines = {"Stand down or be fired upon."};
    aggressive.requiredItem = "security_pass";
    aggressive.minInventoryCount = 1;

    vm.registerHook(polite);
    vm.registerHook(aggressive);
}

} // namespace fuse::adventure
