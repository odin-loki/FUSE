#include <fuse/adventure/adventure_interactable_component.hpp>

#include <fuse/adventure/inventory_component.hpp>
#include <fuse/mechanics/inventory_provider.hpp>

namespace fuse::adventure {

AdventureInteractableComponent::AdventureInteractableComponent(::fuse::adventure::IInteractable& target,
                                                               InteractionSystem& system)
    : m_target(target), m_system(system) {}

AdventureInteractableComponent::AdventureInteractableComponent(std::string name,
                                                               ::fuse::adventure::IInteractable& target,
                                                               InteractionSystem& system)
    : mechanics::InteractableComponent(std::move(name)), m_target(target), m_system(system) {}

mechanics::IInventoryProvider* AdventureInteractableComponent::resolveInventoryProvider(
    const mechanics::InteractionContext& ctx,
    mechanics::Component* instigatorRoot) const {
    if (instigatorRoot == nullptr) {
        instigatorRoot = ctx.instigatorRoot;
    }

    if (instigatorRoot == nullptr) {
        return nullptr;
    }

    auto* iface = instigatorRoot->getInterface<mechanics::InventoryProviderInterface>(
        "mechanics", "inventory");
    if (iface == nullptr || !iface->isValid()) {
        return nullptr;
    }

    return dynamic_cast<mechanics::IInventoryProvider*>(iface->owner());
}

bool AdventureInteractableComponent::canInteract(const mechanics::InteractionContext& ctx) const {
    if (!mechanics::InteractableComponent::canInteract(ctx)) {
        return false;
    }

    if (ctx.verb == "use") {
        if (ctx.item.empty()) {
            return false;
        }

        auto* provider = resolveInventoryProvider(ctx, ctx.instigatorRoot);
        return provider != nullptr && provider->hasItem(ctx.item);
    }

    if (ctx.verb == "pickup") {
        return !ctx.item.empty();
    }

    return false;
}

bool AdventureInteractableComponent::interact(mechanics::InteractionContext& ctx) {
    if (!canInteract(ctx)) {
        return false;
    }

    auto* provider = resolveInventoryProvider(ctx, ctx.instigatorRoot);
    if (provider == nullptr) {
        return false;
    }

    auto* inventoryComponent = dynamic_cast<InventoryComponent*>(provider);
    if (inventoryComponent == nullptr) {
        return false;
    }

    InteractContext adventureCtx;
    adventureCtx.inventory = &inventoryComponent->inventory();
    adventureCtx.actorName = ctx.instigatorRoot != nullptr ? ctx.instigatorRoot->name().c_str() : nullptr;

    const ItemId item(ctx.item);
    InteractResult result = InteractResult::Ignored;

    if (ctx.verb == "use") {
        result = m_system.use(adventureCtx, item, m_target);
    } else if (ctx.verb == "pickup") {
        result = m_system.pickup(adventureCtx, item, ctx.amount, m_target);
    }

    if (result == InteractResult::Used || result == InteractResult::PickedUp) {
        ++m_interactionCount;
        return true;
    }

    return false;
}

} // namespace fuse::adventure
