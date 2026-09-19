#include <fuse/adventure/conversation_interactable.hpp>

namespace fuse::adventure {

ConversationInteractable::ConversationInteractable(std::vector<std::string> lines,
                                                   std::vector<ConversationBranch> branches)
    : m_lines(std::move(lines)), m_branches(std::move(branches)) {}

const std::string& ConversationInteractable::currentLine() const {
    if (m_lineIndex < m_lines.size()) {
        return m_lines[m_lineIndex];
    }
    static const std::string kEmpty;
    return kEmpty;
}

InteractResult ConversationInteractable::converse(InteractContext& ctx) {
    if (m_lines.empty()) {
        return InteractResult::Failed;
    }

    m_lastActor = ctx.actorName != nullptr ? ctx.actorName : "";
    ++m_converseCount;
    if (m_lineIndex + 1 < m_lines.size()) {
        ++m_lineIndex;
    }
    return InteractResult::Examined;
}

InteractResult ConversationInteractable::chooseBranch(InteractContext& ctx, const std::string& branchId) {
    for (const ConversationBranch& branch : m_branches) {
        if (branch.id != branchId) {
            continue;
        }

        m_activeBranchId = branchId;
        m_lines = branch.lines;
        m_lineIndex = 0;
        return converse(ctx);
    }

    return InteractResult::Failed;
}

InteractResult ConversationInteractable::onUse(InteractContext& ctx, ItemId item) {
    (void)item;
    return converse(ctx);
}

InteractResult ConversationInteractable::onPickup(InteractContext& /*ctx*/, ItemId /*item*/, u32 /*amount*/) {
    return InteractResult::Ignored;
}

} // namespace fuse::adventure
