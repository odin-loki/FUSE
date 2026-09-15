#pragma once

#include <fuse/mechanics/component_interface.hpp>
#include <fuse/object.hpp>

#include <string>
#include <vector>

namespace fuse::mechanics {

/// FUSE-native component base (ore: GMK `SimComponent`).
///
/// See `third_party/addons/GMK/Engine/source/component/simComponent.h`.
class Component : public Object {
public:
    Component();
    explicit Component(std::string name);
    ~Component() override;

    const char* typeName() const override { return "Component"; }

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool value) { m_enabled = value; }

    const Component* owner() const { return m_owner; }
    Component* owner() { return m_owner; }

    bool hasComponents() const { return !m_components.empty(); }
    s32 componentCount() const { return static_cast<s32>(m_components.size()); }
    Component* componentAt(s32 index);
    const Component* componentAt(s32 index) const;

    bool addComponent(Component* component);
    bool removeComponent(Component* component);
    void clearComponents();

    /// Mirrors GMK `onAdd` registration chain.
    bool attach();
    void detach();

    bool registerCachedInterface(const char* type,
                                 const char* name,
                                 Component* interfaceOwner,
                                 ComponentInterface* iface);

    bool getInterfaces(ComponentInterfaceList* list,
                       const char* type = nullptr,
                       const char* name = nullptr,
                       const Component* owner = nullptr,
                       bool notOwner = false);

    ComponentInterface* getInterface(const char* type = nullptr,
                                     const char* name = nullptr,
                                     const Component* owner = nullptr,
                                     bool notOwner = false);

    template <typename T>
    T* getInterface(const char* type = nullptr,
                    const char* name = nullptr,
                    const Component* owner = nullptr,
                    bool notOwner = false) {
        ComponentInterfaceList matches;
        if (!getInterfaces(&matches, type, name, owner, notOwner)) {
            return nullptr;
        }

        for (ComponentInterface* iface : matches) {
            if (auto* typed = dynamic_cast<T*>(iface)) {
                return typed;
            }
        }

        return nullptr;
    }

protected:
    /// Called before `onComponentRegister` so dependents can query interfaces.
    virtual void registerInterfaces(Component* owner);

    /// Return false to invalidate the component hierarchy (GMK `onComponentRegister`).
    virtual bool onComponentRegister(Component* owner);

    virtual void onComponentUnregister();

private:
    void registerInterfacesRecursive(Component* owner);
    bool registerComponentsRecursive(Component* owner);
    void unregisterComponentsRecursive();

    std::vector<Component*> m_components;
    Component* m_owner = nullptr;
    ComponentInterfaceCache m_interfaceCache;
    bool m_enabled = true;
};

} // namespace fuse::mechanics
