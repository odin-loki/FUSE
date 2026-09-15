#pragma once

#include <fuse/mechanics/interactable.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::mechanics {

/// Built-in interaction verbs (ore: GMK action dispatch table).
enum class InteractActionKind : u8 {
    Use,
    Pickup,
    Examine,
};

[[nodiscard]] const char* verbForActionKind(InteractActionKind kind);
[[nodiscard]] InteractActionKind actionKindForVerb(const std::string& verb);

/// Typed action payload — converts to/from `InteractionContext` for registry dispatch.
struct InteractAction {
    InteractActionKind kind = InteractActionKind::Use;
    std::string item;
    u32 amount = 1;

    [[nodiscard]] InteractionContext toContext() const;
    [[nodiscard]] static InteractAction fromContext(const InteractionContext& ctx);
};

} // namespace fuse::mechanics
