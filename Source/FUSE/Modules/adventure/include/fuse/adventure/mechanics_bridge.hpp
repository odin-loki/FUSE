#pragma once

#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/mechanics/registry.hpp>

namespace fuse::adventure {

/// Game-thread bridge from mechanics registry dispatch to adventure inventory + interaction APIs.
/// Stateless: each AdventureInteractableComponent carries its own InteractionSystem reference.
class MechanicsBridge {
public:

    InteractResult use(mechanics::MechanicsRegistry& registry,
                       mechanics::Component* target,
                       mechanics::Component* instigator,
                       mechanics::InteractionContext& ctx);

    InteractResult pickup(mechanics::MechanicsRegistry& registry,
                          mechanics::Component* target,
                          mechanics::Component* instigator,
                          mechanics::InteractionContext& ctx);

    InteractResult examine(mechanics::MechanicsRegistry& registry,
                           mechanics::Component* target,
                           mechanics::Component* instigator,
                           mechanics::InteractionContext& ctx);

private:
    InteractContext buildContext(mechanics::Component* instigator) const;
};

} // namespace fuse::adventure
