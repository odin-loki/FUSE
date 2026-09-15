#pragma once

#include <fuse/handle.hpp>
#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/component_interface.hpp>
#include <fuse/object.hpp>

#include <string>
#include <vector>

namespace fuse::mechanics {

class Component;

/// Player/world interaction payload (FUSE mechanics gate; adventure modules share this shape).
struct InteractionContext {
    Handle<Object> instigator = Handle<Object>::invalid();
    Component* instigatorRoot = nullptr;
    std::string verb;
    std::string item;
    u32 amount = 1;
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

    void setSupportedVerbs(std::vector<std::string> verbs);
    const std::vector<std::string>& supportedVerbs() const { return m_supportedVerbs; }

private:
    bool supportsVerb(const std::string& verb) const;

    InteractableInterface m_interactableInterface;
    std::vector<std::string> m_supportedVerbs = {"use", "pickup"};

protected:
    u32 m_interactionCount = 0;
};

} // namespace fuse::mechanics
