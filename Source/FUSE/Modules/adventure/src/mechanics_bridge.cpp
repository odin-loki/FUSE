#include <fuse/adventure/mechanics_bridge.hpp>

#include <fuse/adventure/adventure_interactable_component.hpp>
#include <fuse/adventure/inventory_component.hpp>

namespace fuse::adventure {

MechanicsBridge::MechanicsBridge(InteractionSystem& system) : m_system(system) {}

InteractContext MechanicsBridge::buildContext(mechanics::Component* instigator) const {
    InteractContext ctx;
    ctx.actorName = instigator != nullptr ? instigator->name().c_str() : nullptr;

    if (instigator == nullptr) {
        return ctx;
    }

    auto* iface = instigator->getInterface<mechanics::InventoryProviderInterface>(
        "mechanics", "inventory");
    if (iface == nullptr || !iface->isValid()) {
        return ctx;
    }

    auto* inventoryComponent = dynamic_cast<InventoryComponent*>(iface->owner());
    if (inventoryComponent != nullptr) {
        ctx.inventory = &inventoryComponent->inventory();
    }

    return ctx;
}

InteractResult MechanicsBridge::use(mechanics::MechanicsRegistry& registry,
                                    mechanics::Component* target,
                                    mechanics::Component* instigator,
                                    mechanics::InteractionContext& ctx) {
    ctx.instigatorRoot = instigator;
    ctx.verb = "use";

    if (!registry.interact(target, ctx)) {
        return InteractResult::Failed;
    }

    return InteractResult::Used;
}

InteractResult MechanicsBridge::pickup(mechanics::MechanicsRegistry& registry,
                                       mechanics::Component* target,
                                       mechanics::Component* instigator,
                                       mechanics::InteractionContext& ctx) {
    ctx.instigatorRoot = instigator;
    ctx.verb = "pickup";

    if (!registry.interact(target, ctx)) {
        return InteractResult::Failed;
    }

    return InteractResult::PickedUp;
}

} // namespace fuse::adventure
