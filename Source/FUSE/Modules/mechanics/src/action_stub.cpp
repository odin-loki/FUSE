#include <fuse/mechanics/action_stub.hpp>

namespace fuse::mechanics {

void IActionStub::recordExecution(const InteractionContext& ctx) {
    ++m_executionCount;
    m_lastItem = ctx.item;
}

ActionStubComponent::ActionStubComponent(InteractActionKind kind) : m_kind(kind) {}

ActionStubComponent::ActionStubComponent(std::string name, InteractActionKind kind)
    : Component(std::move(name)), m_kind(kind) {}

bool ActionStubComponent::canExecute(const InteractionContext& ctx) const {
    if (!isEnabled()) {
        return false;
    }

    if (ctx.verb.empty()) {
        return true;
    }

    return ctx.verb == verbForActionKind(m_kind);
}

bool ActionStubComponent::execute(InteractionContext& ctx) {
    if (!canExecute(ctx)) {
        return false;
    }

    recordExecution(ctx);
    return true;
}

UseActionStub::UseActionStub() : ActionStubComponent(InteractActionKind::Use) {}

UseActionStub::UseActionStub(std::string name)
    : ActionStubComponent(std::move(name), InteractActionKind::Use) {}

PickupActionStub::PickupActionStub() : ActionStubComponent(InteractActionKind::Pickup) {}

PickupActionStub::PickupActionStub(std::string name)
    : ActionStubComponent(std::move(name), InteractActionKind::Pickup) {}

ExamineActionStub::ExamineActionStub() : ActionStubComponent(InteractActionKind::Examine) {}

ExamineActionStub::ExamineActionStub(std::string name)
    : ActionStubComponent(std::move(name), InteractActionKind::Examine) {}

} // namespace fuse::mechanics
