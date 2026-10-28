#pragma once

// Named quest flags. A name is either set or absent; unknown names read as unset.
// set and clear ignore an empty name, and has returns false for it.
// all_of is true for an empty list. any_of is false for an empty list.
// names() returns the currently set flags in sorted order.

#include <functional>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::gameplay {

class QuestFlags {
public:
    void set(std::string_view name) {
        if (name.empty()) {
            return;
        }
        m_flags.emplace(name);
    }

    void clear(std::string_view name) {
        if (name.empty()) {
            return;
        }
        const auto it = m_flags.find(name);
        if (it != m_flags.end()) {
            m_flags.erase(it);
        }
    }

    [[nodiscard]] bool has(std::string_view name) const {
        if (name.empty()) {
            return false;
        }
        return m_flags.find(name) != m_flags.end();
    }

    /// True when every listed flag is set. An empty list is true.
    [[nodiscard]] bool all_of(std::initializer_list<std::string_view> names) const {
        for (const std::string_view name : names) {
            if (!has(name)) {
                return false;
            }
        }
        return true;
    }

    /// True when at least one listed flag is set. An empty list is false.
    [[nodiscard]] bool any_of(std::initializer_list<std::string_view> names) const {
        for (const std::string_view name : names) {
            if (has(name)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> names() const {
        return std::vector<std::string>(m_flags.begin(), m_flags.end());
    }

private:
    std::set<std::string, std::less<>> m_flags;
};

} // namespace fuse::gameplay
