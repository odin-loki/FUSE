#include <fuse/mechanics/component_interface.hpp>

#include <algorithm>

namespace fuse::mechanics {

namespace {

bool matchesField(const char* query, const std::string& value) {
    if (query == nullptr || query[0] == '\0') {
        return true;
    }
    return value == query;
}

} // namespace

bool ComponentInterfaceCache::add(const char* type,
                                const char* name,
                                const Component* owner,
                                ComponentInterface* iface) {
    if (iface == nullptr) {
        return false;
    }

    const std::string typeKey = type != nullptr ? type : "";
    const std::string nameKey = name != nullptr ? name : "";

    const auto duplicate = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& entry) {
        return entry.owner == owner && entry.type == typeKey && entry.name == nameKey;
    });
    if (duplicate != m_entries.end()) {
        return false;
    }

    m_entries.push_back({iface, typeKey, nameKey, owner});
    return true;
}

void ComponentInterfaceCache::clear() {
    m_entries.clear();
}

u32 ComponentInterfaceCache::enumerate(ComponentInterfaceList* list,
                                         const char* type,
                                         const char* name,
                                         const Component* owner,
                                         bool notOwner) const {
    u32 matches = 0;

    for (const Entry& entry : m_entries) {
        if (owner != nullptr) {
            const bool ownedByQuery = entry.owner == owner;
            if ((ownedByQuery && notOwner) || (!ownedByQuery && !notOwner)) {
                continue;
            }
        }

        if (!matchesField(type, entry.type) || !matchesField(name, entry.name)) {
            continue;
        }

        ++matches;
        if (list != nullptr) {
            list->push_back(entry.iface);
        }
    }

    return matches;
}

} // namespace fuse::mechanics
