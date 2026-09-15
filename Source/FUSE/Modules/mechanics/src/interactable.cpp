#include <fuse/mechanics/interactable.hpp>

#include <algorithm>

namespace fuse::mechanics {

bool InteractableInterface::canInteract(const InteractionContext& ctx) const {
    if (!isValid()) {
        return false;
    }

    auto* component = dynamic_cast<const InteractableComponent*>(owner());
    if (component == nullptr) {
        return false;
    }

    return component->canInteract(ctx);
}

bool InteractableInterface::interact(InteractionContext& ctx) {
    if (!isValid()) {
        return false;
    }

    auto* component = dynamic_cast<InteractableComponent*>(owner());
    if (component == nullptr) {
        return false;
    }

    return component->interact(ctx);
}

InteractableComponent::InteractableComponent() = default;

InteractableComponent::InteractableComponent(std::string name) : Component(std::move(name)) {}

void InteractableComponent::registerInterfaces(Component* owner) {
    Component::registerInterfaces(owner);
    owner->registerCachedInterface("mechanics", "interactable", this, &m_interactableInterface);
}

void InteractableComponent::setSupportedVerbs(std::vector<std::string> verbs) {
    m_supportedVerbs = std::move(verbs);
}

bool InteractableComponent::supportsVerb(const std::string& verb) const {
    if (verb.empty()) {
        return true;
    }

    return std::find(m_supportedVerbs.begin(), m_supportedVerbs.end(), verb) != m_supportedVerbs.end();
}

bool InteractableComponent::canInteract(const InteractionContext& ctx) const {
    if (!isEnabled()) {
        return false;
    }

    return supportsVerb(ctx.verb);
}

bool InteractableComponent::interact(InteractionContext& ctx) {
    if (!canInteract(ctx)) {
        return false;
    }

    ++m_interactionCount;
    return true;
}

} // namespace fuse::mechanics
