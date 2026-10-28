#pragma once

#include <cstdint>

namespace fuse::core_logic {

inline std::uint32_t pages_needed(std::uint64_t bytes, std::uint32_t pageBytes) {
    if (pageBytes == 0) {
        return 0;
    }
    const std::uint64_t pages =
        (bytes + static_cast<std::uint64_t>(pageBytes) - 1ull) / static_cast<std::uint64_t>(pageBytes);
    if (pages > 0xffffffffull) {
        return 0xffffffffu;
    }
    return static_cast<std::uint32_t>(pages);
}

inline bool pages_fit(std::uint32_t needed, std::uint32_t used, std::uint32_t cap) {
    return static_cast<std::uint64_t>(used) + static_cast<std::uint64_t>(needed) <= static_cast<std::uint64_t>(cap);
}

} // namespace fuse::core_logic
