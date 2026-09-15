#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::mechanics {

class Component;

/// Thin capability surface exposed by a component (ore: GMK `ComponentInterface`).
///
/// See `third_party/addons/GMK/Engine/source/component/componentInterface.h`.
class ComponentInterface {
public:
    virtual ~ComponentInterface() = default;

    virtual bool isValid() const { return m_owner != nullptr; }

    Component* owner() { return m_owner; }
    const Component* owner() const { return m_owner; }

protected:
    void setOwner(Component* owner) { m_owner = owner; }

private:
    Component* m_owner = nullptr;

    friend class Component;
};

using ComponentInterfaceList = std::vector<ComponentInterface*>;

/// Cached interface lookup table (ore: GMK `ComponentInterfaceCache`).
class ComponentInterfaceCache {
public:
    bool add(const char* type,
             const char* name,
             const Component* owner,
             ComponentInterface* iface);

    void clear();

    /// Appends matches to `list` when non-null. Returns match count.
    u32 enumerate(ComponentInterfaceList* list,
                  const char* type = nullptr,
                  const char* name = nullptr,
                  const Component* owner = nullptr,
                  bool notOwner = false) const;

private:
    struct Entry {
        ComponentInterface* iface = nullptr;
        std::string type;
        std::string name;
        const Component* owner = nullptr;
    };

    std::vector<Entry> m_entries;
};

} // namespace fuse::mechanics
