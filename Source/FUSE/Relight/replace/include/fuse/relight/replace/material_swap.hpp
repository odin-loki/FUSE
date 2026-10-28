#pragma once

#include <cstddef>
#include <string>

namespace fuse::relight::replace {

struct MaterialSwapRule {
    std::string from;
    std::string to;
};

/// First rule whose `from` equals `name`. An empty `from` never matches, and a later rule does not override
/// an earlier match. A null `name` returns empty; no match returns `name`.
[[nodiscard]] inline std::string apply_material_swap(const char* name, const MaterialSwapRule* rules,
                                                      std::size_t count) {
    if (name == nullptr) {
        return {};
    }
    if (rules != nullptr) {
        for (std::size_t i = 0; i < count; ++i) {
            if (rules[i].from.empty()) {
                continue;
            }
            if (rules[i].from == name) {
                return rules[i].to;
            }
        }
    }
    return std::string(name);
}

} // namespace fuse::relight::replace
