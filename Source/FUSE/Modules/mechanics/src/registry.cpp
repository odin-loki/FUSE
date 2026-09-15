#include <fuse/mechanics/registry.hpp>

#include <algorithm>

namespace fuse::mechanics {

namespace {

IInteractable* resolveFromComponentTree(Component* root) {
    if (root == nullptr) {
        return nullptr;
    }

    if (auto* direct = dynamic_cast<IInteractable*>(root)) {
        return direct;
    }

    auto* iface = root->getInterface<InteractableInterface>("mechanics", "interactable");
    if (iface != nullptr && iface->isValid()) {
        return dynamic_cast<IInteractable*>(iface->owner());
    }

    return nullptr;
}

const IInteractable* resolveFromComponentTree(const Component* root) {
    return const_cast<const IInteractable*>(resolveFromComponentTree(const_cast<Component*>(root)));
}

} // namespace

IInteractable* MechanicsRegistry::resolveInteractable(Component* root) {
    return resolveFromComponentTree(root);
}

const IInteractable* MechanicsRegistry::resolveInteractable(const Component* root) {
    return resolveFromComponentTree(root);
}

void MechanicsRegistry::registerInteractable(Object* target, IInteractable* interactable) {
    if (target == nullptr || interactable == nullptr) {
        return;
    }

    unregisterInteractable(target);
    m_entries.push_back({target, interactable});
}

void MechanicsRegistry::unregisterInteractable(Object* target) {
    const auto it = std::remove_if(m_entries.begin(), m_entries.end(), [target](const Entry& entry) {
        return entry.target == target;
    });
    m_entries.erase(it, m_entries.end());
}

bool MechanicsRegistry::interact(Object* target, InteractionContext ctx) {
    IInteractable* interactable = findInteractable(target);
    if (interactable == nullptr) {
        return false;
    }

    if (!interactable->canInteract(ctx)) {
        return false;
    }

    return interactable->interact(ctx);
}

bool MechanicsRegistry::canInteract(Object* target, const InteractionContext& ctx) const {
    const IInteractable* interactable = findInteractable(target);
    if (interactable == nullptr) {
        return false;
    }

    return interactable->canInteract(ctx);
}

IInteractable* MechanicsRegistry::findInteractable(Object* target) {
    if (target == nullptr) {
        return nullptr;
    }

    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [target](const Entry& entry) {
        return entry.target == target;
    });

    if (it == m_entries.end()) {
        return nullptr;
    }

    return it->interactable;
}

const IInteractable* MechanicsRegistry::findInteractable(const Object* target) const {
    if (target == nullptr) {
        return nullptr;
    }

    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [target](const Entry& entry) {
        return entry.target == target;
    });

    if (it == m_entries.end()) {
        return nullptr;
    }

    return it->interactable;
}

IInteractable* MechanicsRegistry::findInteractable(Component* target) {
    if (target == nullptr) {
        return nullptr;
    }

    IInteractable* explicitEntry = findInteractable(static_cast<Object*>(target));
    if (explicitEntry != nullptr) {
        return explicitEntry;
    }

    return resolveInteractable(target);
}

const IInteractable* MechanicsRegistry::findInteractable(const Component* target) const {
    if (target == nullptr) {
        return nullptr;
    }

    const IInteractable* explicitEntry = findInteractable(static_cast<const Object*>(target));
    if (explicitEntry != nullptr) {
        return explicitEntry;
    }

    return resolveInteractable(target);
}

bool MechanicsRegistry::interact(Component* target, InteractionContext ctx) {
    IInteractable* interactable = findInteractable(target);
    if (interactable == nullptr) {
        return false;
    }

    if (!interactable->canInteract(ctx)) {
        return false;
    }

    return interactable->interact(ctx);
}

bool MechanicsRegistry::canInteract(Component* target, const InteractionContext& ctx) const {
    const IInteractable* interactable = findInteractable(target);
    if (interactable == nullptr) {
        return false;
    }

    return interactable->canInteract(ctx);
}

} // namespace fuse::mechanics
