#include <fuse/mechanics/interact_action.hpp>

namespace fuse::mechanics {

const char* verbForActionKind(InteractActionKind kind) {
    switch (kind) {
    case InteractActionKind::Use:
        return "use";
    case InteractActionKind::Pickup:
        return "pickup";
    case InteractActionKind::Examine:
        return "examine";
    }

    return "";
}

InteractActionKind actionKindForVerb(const std::string& verb) {
    if (verb == "use") {
        return InteractActionKind::Use;
    }
    if (verb == "pickup") {
        return InteractActionKind::Pickup;
    }
    if (verb == "examine") {
        return InteractActionKind::Examine;
    }

    return InteractActionKind::Use;
}

InteractionContext InteractAction::toContext() const {
    InteractionContext ctx;
    ctx.verb = verbForActionKind(kind);
    ctx.item = item;
    ctx.amount = amount;
    return ctx;
}

InteractAction InteractAction::fromContext(const InteractionContext& ctx) {
    InteractAction action;
    action.kind = actionKindForVerb(ctx.verb);
    action.item = ctx.item;
    action.amount = ctx.amount;
    return action;
}

} // namespace fuse::mechanics
