#include <fuse/legacy/string_intern_table.hpp>

namespace fuse::legacy {

u32 StringInternTable::intern(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }

    const std::string key(value);
    const auto found = m_index.find(key);
    if (found != m_index.end()) {
        return found->second;
    }

    const u32 id = static_cast<u32>(m_storage.size() + 1);
    m_storage.push_back(key);
    m_index.emplace(m_storage.back(), id);
    return id;
}

u32 StringInternTable::entryCount() const {
    return static_cast<u32>(m_storage.size());
}

const char* StringInternTable::lookup(u32 id) const {
    if (id == 0 || id > m_storage.size()) {
        return nullptr;
    }
    return m_storage[id - 1].c_str();
}

} // namespace fuse::legacy
