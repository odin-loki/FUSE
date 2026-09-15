#pragma once

#include <fuse/mechanics/interactable.hpp>

#include <vector>

namespace fuse::mechanics {

/// Stub registry for interactable lookup and dispatch.
class MechanicsRegistry {
public:
    void registerInteractable(Object* target, IInteractable* interactable);
    void unregisterInteractable(Object* target);

    bool interact(Object* target, InteractionContext ctx);
    bool canInteract(Object* target, const InteractionContext& ctx) const;

    u32 interactableCount() const { return static_cast<u32>(m_entries.size()); }

private:
    struct Entry {
        Object* target = nullptr;
        IInteractable* interactable = nullptr;
    };

    IInteractable* findInteractable(Object* target) const;

    std::vector<Entry> m_entries;
};

} // namespace fuse::mechanics
