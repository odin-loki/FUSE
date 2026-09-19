#include <fuse/adventure/examine_interactable.hpp>

namespace fuse::adventure {

ExamineInteractable::ExamineInteractable(std::string description)
    : m_description(std::move(description)) {}

InteractResult ExamineInteractable::onUse(InteractContext& ctx, ItemId item) {
    (void)item;
    return onExamine(ctx);
}

InteractResult ExamineInteractable::onPickup(InteractContext& /*ctx*/, ItemId /*item*/, u32 /*amount*/) {
    return InteractResult::Ignored;
}

InteractResult ExamineInteractable::onExamine(InteractContext& ctx) {
    ++m_examineCount;
    m_lastExaminedBy = ctx.actorName != nullptr ? ctx.actorName : "";
    return InteractResult::Examined;
}

} // namespace fuse::adventure
