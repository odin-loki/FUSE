#pragma once

#include <stdint.h>

namespace fuse::core_logic {

inline uint32_t pages_needed(uint64_t bytes, uint32_t pageBytes) {
    if (pageBytes == 0) {
        return 0;
    }
    const uint64_t pages =
        (bytes + static_cast<uint64_t>(pageBytes) - 1ull) / static_cast<uint64_t>(pageBytes);
    if (pages > 0xffffffffull) {
        return 0xffffffffu;
    }
    return static_cast<uint32_t>(pages);
}

inline bool pages_fit(uint32_t needed, uint32_t used, uint32_t cap) {
    return static_cast<uint64_t>(used) + static_cast<uint64_t>(needed) <= static_cast<uint64_t>(cap);
}

} // namespace fuse::core_logic
