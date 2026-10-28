#pragma once

#include <cstring>

namespace fuse::renderer {

/// True when every non-null `need` name strcmp-matches a non-null `have` name.
/// `needCount <= 0` is satisfied. A null `need` with `needCount > 0` is not.
/// A null `have` or `haveCount <= 0` is not satisfied when any required name is non-null.
/// Null entries in either list are ignored.
[[nodiscard]] inline bool extensions_satisfied(const char* const* have, int haveCount,
                                                const char* const* need, int needCount) {
    if (needCount <= 0) {
        return true;
    }
    if (need == nullptr) {
        return false;
    }

    const bool haveUsable = have != nullptr && haveCount > 0;
    for (int i = 0; i < needCount; ++i) {
        const char* required = need[i];
        if (required == nullptr) {
            continue;
        }
        if (!haveUsable) {
            return false;
        }
        bool found = false;
        for (int j = 0; j < haveCount; ++j) {
            const char* present = have[j];
            if (present != nullptr && std::strcmp(required, present) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

} // namespace fuse::renderer
