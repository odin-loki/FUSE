#pragma once

namespace fuse::mechanics {

inline bool provider_can_give(int have, int requested) {
    if (have < 0) {
        have = 0;
    }
    return requested > 0 && have >= requested;
}

inline int provider_left(int have, int requested) {
    if (have < 0) {
        have = 0;
    }
    if (requested <= 0 || have < requested) {
        return have;
    }
    return have - requested;
}

} // namespace fuse::mechanics
