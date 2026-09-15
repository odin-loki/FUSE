#include <fuse/mechanics/component.hpp>

#include <algorithm>

namespace fuse::mechanics {

Component::Component() = default;

Component::Component(std::string name) : Object(std::move(name)) {}

Component::~Component() {
    detach();
}

Component* Component::componentAt(s32 index) {
    if (index < 0 || index >= componentCount()) {
        return nullptr;
    }
    return m_components[static_cast<size_t>(index)];
}

const Component* Component::componentAt(s32 index) const {
    if (index < 0 || index >= componentCount()) {
        return nullptr;
    }
    return m_components[static_cast<size_t>(index)];
}

bool Component::addComponent(Component* component) {
    if (component == nullptr || component == this) {
        return false;
    }

    if (std::find(m_components.begin(), m_components.end(), component) != m_components.end()) {
        return false;
    }

    m_components.push_back(component);
    return true;
}

bool Component::removeComponent(Component* component) {
    const auto it = std::find(m_components.begin(), m_components.end(), component);
    if (it == m_components.end()) {
        return false;
    }

    m_components.erase(it);
    return true;
}

void Component::clearComponents() {
    m_components.clear();
}

bool Component::attach() {
    registerInterfacesRecursive(this);
    return registerComponentsRecursive(this);
}

void Component::detach() {
    unregisterComponentsRecursive();
    m_interfaceCache.clear();
    m_owner = nullptr;
}

bool Component::registerCachedInterface(const char* type,
                                          const char* name,
                                          Component* interfaceOwner,
                                          ComponentInterface* iface) {
    if (interfaceOwner == nullptr || iface == nullptr) {
        return false;
    }

    iface->setOwner(interfaceOwner);
    if (!m_interfaceCache.add(type, name, interfaceOwner, iface)) {
        return false;
    }

    if (m_owner != nullptr) {
        return m_owner->registerCachedInterface(type, name, interfaceOwner, iface);
    }

    return true;
}

bool Component::getInterfaces(ComponentInterfaceList* list,
                              const char* type,
                              const char* name,
                              const Component* owner,
                              bool notOwner) {
    u32 matches = m_interfaceCache.enumerate(list, type, name, owner, notOwner);

    for (Component* child : m_components) {
        matches += child->getInterfaces(list, type, name, owner, notOwner);
    }

    return matches > 0;
}

ComponentInterface* Component::getInterface(const char* type,
                                            const char* name,
                                            const Component* owner,
                                            bool notOwner) {
    ComponentInterfaceList matches;
    if (!getInterfaces(&matches, type, name, owner, notOwner) || matches.empty()) {
        return nullptr;
    }
    return matches.front();
}

void Component::registerInterfaces(Component* owner) {
    (void)owner;
}

bool Component::onComponentRegister(Component* owner) {
    m_owner = owner;
    return true;
}

void Component::onComponentUnregister() {
    m_owner = nullptr;
}

void Component::registerInterfacesRecursive(Component* owner) {
    registerInterfaces(owner);

    for (Component* child : m_components) {
        child->registerInterfaces(owner);
        child->registerInterfacesRecursive(owner);
    }
}

bool Component::registerComponentsRecursive(Component* owner) {
    for (Component* child : m_components) {
        if (!child->onComponentRegister(owner)) {
            return false;
        }

        if (!child->registerComponentsRecursive(owner)) {
            return false;
        }
    }

    return true;
}

void Component::unregisterComponentsRecursive() {
    for (Component* child : m_components) {
        child->unregisterComponentsRecursive();
        child->onComponentUnregister();
    }
}

} // namespace fuse::mechanics
