#pragma once

#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/mechanics/registry.hpp>

namespace fuse::adventure {

/// Game-thread bridge from mechanics registry dispatch to adventure inventory + interaction APIs.
class MechanicsBridge {
public:
    explicit MechanicsBridge(InteractionSystem& system);

    InteractResult use(mechanics::MechanicsRegistry& registry,
                       mechanics::Component* target,
                       mechanics::Component* instigator,
                       mechanics::InteractionContext& ctx);

    InteractResult pickup(mechanics::MechanicsRegistry& registry,
                          mechanics::Component* target,
                          mechanics::Component* instigator,
                          mechanics::InteractionContext& ctx);

private:
    InteractContext buildContext(mechanics::Component* instigator) const;

    InteractionSystem& m_system;
};

} // namespace fuse::adventure
