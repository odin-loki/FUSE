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

u32 ConversationScriptVm::branchLineCount(const std::string& npcId, const std::string& branchId) const {
    const auto it = m_hooks.find(hookKey(npcId, branchId));
    if (it == m_hooks.end()) {
        return 0;
    }
    return static_cast<u32>(it->second.lines.size());
}

std::string ConversationScriptVm::peekBranchLine(const std::string& npcId,
                                                 const std::string& branchId,
                                                 u32 lineIndex) const {
    const auto it = m_hooks.find(hookKey(npcId, branchId));
    if (it == m_hooks.end() || lineIndex >= it->second.lines.size()) {
        return {};
    }
    return it->second.lines[lineIndex];
}

bool ConversationScriptVm::inventoryMeetsRequirements(const ConversationScriptHook& hook,
                                                       const InteractContext& ctx) const {
    if (ctx.inventory == nullptr) {
        return hook.requiredItem.empty() && hook.elifRequiredItems.empty();
    }

    if (!hook.requiredItem.empty()) {
        if (ctx.inventory->getInventory(ItemId(hook.requiredItem)) >= hook.minInventoryCount) {
            return true;
        }
    } else if (hook.elifRequiredItems.empty()) {
        return true;
    }

    for (const auto& elifReq : hook.elifRequiredItems) {
        if (ctx.inventory->getInventory(ItemId(elifReq.first)) >= elifReq.second) {
            return true;
        }
    }

    return hook.requiredItem.empty() && hook.elifRequiredItems.empty();
}

bool ConversationScriptVm::canDispatchBranch(const std::string& npcId,
                                              const std::string& branchId,
                                              const InteractContext& ctx) const {
    const auto it = m_hooks.find(hookKey(npcId, branchId));
    if (it == m_hooks.end()) {
        return false;
    }

    return inventoryMeetsRequirements(it->second, ctx);
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

    if (!it->second.grantItem.empty() && ctx.inventory != nullptr && it->second.grantAmount > 0) {
        ctx.inventory->incInventory(ItemId(it->second.grantItem), it->second.grantAmount);
        m_lastGrantedItem = it->second.grantItem;
        ++m_grantCount;
    }

    return target.chooseBranch(ctx, branchId) == InteractResult::Examined;
}

u32 ConversationScriptVm::dispatchAllLines(const std::string& npcId,
                                           const std::string& branchId,
                                           InteractContext& ctx,
                                           ConversationInteractable& target) {
    const auto it = m_hooks.find(hookKey(npcId, branchId));
    if (it == m_hooks.end()) {
        return 0;
    }

    if (!canDispatchBranch(npcId, branchId, ctx)) {
        return 0;
    }

    u32 dispatched = 0;
    for (const std::string& line : it->second.lines) {
        m_lastLineDispatched = line;
        ++dispatched;
        ++m_lineDispatchCount;
    }

    if (dispatchBranch(npcId, branchId, ctx, target)) {
        if (!it->second.lines.empty()) {
            m_lastLineDispatched = it->second.lines.back();
        }
        return dispatched;
    }
    return 0;
}

bool ConversationScriptVm::chooseHighestPriorityBranch(const std::string& npcId,
                                                       const InteractContext& ctx,
                                                       std::string& outBranchId) const {
    u32 bestPriority = 0;
    bool found = false;
    for (const auto& entry : m_hooks) {
        const ConversationScriptHook& hook = entry.second;
        if (hook.npcId != npcId) {
            continue;
        }
        if (!canDispatchBranch(npcId, hook.branchId, ctx)) {
            continue;
        }
        if (!found || hook.priority > bestPriority) {
            bestPriority = hook.priority;
            outBranchId = hook.branchId;
            found = true;
        }
    }
    return found;
}

bool ConversationScriptVm::dispatchBestBranch(const std::string& npcId,
                                              InteractContext& ctx,
                                              ConversationInteractable& target) {
    std::string branchId;
    if (!chooseHighestPriorityBranch(npcId, ctx, branchId)) {
        return false;
    }
    return dispatchBranch(npcId, branchId, ctx, target);
}

std::string ConversationScriptVm::peekBestBranchLine(const std::string& npcId,
                                                     const InteractContext& ctx) const {
    std::string branchId;
    if (!chooseHighestPriorityBranch(npcId, ctx, branchId)) {
        return {};
    }
    return peekBranchLine(npcId, branchId, 0);
}

u32 ConversationScriptVm::dispatchBestBranchAllLines(const std::string& npcId,
                                                     InteractContext& ctx,
                                                     ConversationInteractable& target) {
    std::string branchId;
    if (!chooseHighestPriorityBranch(npcId, ctx, branchId)) {
        return 0;
    }
    return dispatchAllLines(npcId, branchId, ctx, target);
}

void registerOutpostConversationScriptHooks(ConversationScriptVm& vm) {
    ConversationScriptHook polite{};
    polite.npcId = "outpost_guard";
    polite.branchId = "polite";
    polite.lines = {"Thank you, traveler. Proceed with caution."};

    ConversationScriptHook aggressive{};
    aggressive.npcId = "outpost_guard";
    aggressive.branchId = "aggressive";
    aggressive.lines = {"Stand down or be fired upon.", "You may pass — this time."};
    aggressive.requiredItem = "security_pass";
    aggressive.minInventoryCount = 1;
    aggressive.grantItem = "security_badge";
    aggressive.grantAmount = 1;

    ConversationScriptHook armed{};
    armed.npcId = "outpost_guard";
    armed.branchId = "armed";
    armed.lines = {"I see you are armed. Keep that rifle stowed."};
    armed.requiredItem = "plasma_rifle";
    armed.minInventoryCount = 1;
    armed.priority = 10;

    vm.registerHook(polite);
    vm.registerHook(aggressive);
    vm.registerHook(armed);
}

} // namespace fuse::adventure
