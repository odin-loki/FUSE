#pragma once

#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/interactable.hpp>
#include <fuse/mechanics/interactable.hpp>
#include <fuse/mechanics/inventory_provider.hpp>

namespace fuse::adventure {

/// Bridges adventure `IInteractable` + `InteractionSystem` into mechanics `InteractableComponent`.
class AdventureInteractableComponent : public mechanics::InteractableComponent {
public:
    AdventureInteractableComponent(::fuse::adventure::IInteractable& target, InteractionSystem& system);
    explicit AdventureInteractableComponent(std::string name,
                                            ::fuse::adventure::IInteractable& target,
                                            InteractionSystem& system);

    const char* typeName() const override { return "AdventureInteractableComponent"; }

    bool canInteract(const mechanics::InteractionContext& ctx) const override;
    bool interact(mechanics::InteractionContext& ctx) override;

    ::fuse::adventure::IInteractable& target() { return m_target; }
    const ::fuse::adventure::IInteractable& target() const { return m_target; }

private:
    mechanics::IInventoryProvider* resolveInventoryProvider(
        const mechanics::InteractionContext& ctx,
        mechanics::Component* instigatorRoot) const;

    ::fuse::adventure::IInteractable& m_target;
    InteractionSystem& m_system;
};

} // namespace fuse::adventure
