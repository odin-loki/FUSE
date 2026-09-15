#pragma once

#include <fuse/handle.hpp>
#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/component_interface.hpp>
#include <fuse/object.hpp>

#include <string>

namespace fuse::mechanics {

/// Player/world interaction payload (FUSE mechanics gate; adventure modules share this shape).
struct InteractionContext {
    Handle<Object> instigator = Handle<Object>::invalid();
    std::string verb;
};

/// Gameplay interaction surface for mechanics-driven objects.
class IInteractable {
public:
    virtual ~IInteractable() = default;

    virtual bool canInteract(const InteractionContext& ctx) const = 0;
    virtual bool interact(InteractionContext& ctx) = 0;
};

/// Cached `IInteractable` accessor (ore: GMK `SimpleComponentInterface`).
class InteractableInterface : public ComponentInterface {
public:
    bool canInteract(const InteractionContext& ctx) const;
    bool interact(InteractionContext& ctx);
};

/// Reference interactable component with cached interface registration.
///
/// Ore pattern: `third_party/addons/GMK/Engine/source/component/simpleComponent.h`.
class InteractableComponent : public Component, public IInteractable {
public:
    InteractableComponent();
    explicit InteractableComponent(std::string name);

    const char* typeName() const override { return "InteractableComponent"; }

    void registerInterfaces(Component* owner) override;

    bool canInteract(const InteractionContext& ctx) const override;
    bool interact(InteractionContext& ctx) override;

    u32 interactionCount() const { return m_interactionCount; }

private:
    InteractableInterface m_interactableInterface;
    u32 m_interactionCount = 0;
};

} // namespace fuse::mechanics
