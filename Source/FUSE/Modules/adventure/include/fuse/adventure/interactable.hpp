#pragma once

#include <fuse/adventure/item_id.hpp>
#include <fuse/types.hpp>

namespace fuse::adventure {

class Inventory;

/// Actor context passed to interaction callbacks (maps to 3DAAK `%user` / `%this`).
struct InteractContext {
    Inventory* inventory = nullptr;
    const char* actorName = nullptr;
};

/// Result of an interaction attempt.
enum class InteractResult {
    Ignored,
    Used,
    PickedUp,
    Failed,
};

/// Base hook for world objects that accept use / pickup (3DAAK `onUse` / `onPickup`).
class IInteractable {
public:
    virtual ~IInteractable() = default;

    virtual InteractResult onUse(InteractContext& ctx, ItemId item) = 0;
    virtual InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) = 0;
};

/// No-op interactable for scaffolding and tests.
class InteractableStub : public IInteractable {
public:
    InteractResult onUse(InteractContext& ctx, ItemId item) override;
    InteractResult onPickup(InteractContext& ctx, ItemId item, u32 amount) override;

    u32 useCount() const { return m_useCount; }
    u32 pickupCount() const { return m_pickupCount; }
    ItemId lastItem() const { return m_lastItem; }

private:
    u32 m_useCount = 0;
    u32 m_pickupCount = 0;
    ItemId m_lastItem;
};

} // namespace fuse::adventure
