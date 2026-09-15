#pragma once

#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/interactable.hpp>

#include <vector>

namespace fuse::mechanics {

/// Registry for interactable lookup and dispatch.
class MechanicsRegistry {
public:
    void registerInteractable(Object* target, IInteractable* interactable);
    void unregisterInteractable(Object* target);

    bool interact(Object* target, InteractionContext ctx);
    bool canInteract(Object* target, const InteractionContext& ctx) const;

    /// Resolve an interactable from explicit registration or component interfaces.
    static IInteractable* resolveInteractable(Component* root);
    static const IInteractable* resolveInteractable(const Component* root);

    bool interact(Component* target, InteractionContext ctx);
    bool canInteract(Component* target, const InteractionContext& ctx) const;

    u32 interactableCount() const { return static_cast<u32>(m_entries.size()); }

private:
    struct Entry {
        Object* target = nullptr;
        IInteractable* interactable = nullptr;
    };

    IInteractable* findInteractable(Object* target);
    const IInteractable* findInteractable(const Object* target) const;
    IInteractable* findInteractable(Component* target);
    const IInteractable* findInteractable(const Component* target) const;

    std::vector<Entry> m_entries;
};

} // namespace fuse::mechanics
