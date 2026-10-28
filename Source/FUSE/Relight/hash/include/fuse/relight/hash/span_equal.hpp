#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fuse::relight::hash {

inline bool spans_equal(const std::uint8_t* a, std::size_t aCount, const std::uint8_t* b, std::size_t bCount) {
    if (aCount != bCount) {
        return false;
    }
    if (aCount == 0) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return std::memcmp(a, b, aCount) == 0;
}

} // namespace fuse::relight::hash
