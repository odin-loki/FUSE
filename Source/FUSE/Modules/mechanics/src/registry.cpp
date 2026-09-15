#include <fuse/mechanics/registry.hpp>

#include <algorithm>

namespace fuse::mechanics {

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

IInteractable* MechanicsRegistry::findInteractable(Object* target) const {
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

} // namespace fuse::mechanics
