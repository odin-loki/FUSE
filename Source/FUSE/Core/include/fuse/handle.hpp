#pragma once

#include <fuse/types.hpp>

#include <cstdint>

namespace fuse {

/// Generation/epoch handle stub (Track A P4 / WP-04).
/// Workers publish handles; the game thread resolves through HandleTable (future).
template <typename T>
class Handle {
public:
    Handle() = default;
    explicit Handle(u32 index, u32 generation) : m_index(index), m_generation(generation) {}

    bool isValid() const { return m_index != kInvalidIndex; }

    u32 index() const { return m_index; }
    u32 generation() const { return m_generation; }

    bool operator==(const Handle& other) const {
        return m_index == other.m_index && m_generation == other.m_generation;
    }

    bool operator!=(const Handle& other) const { return !(*this == other); }

    static Handle invalid() { return Handle{}; }

private:
    static constexpr u32 kInvalidIndex = UINT32_MAX;
    u32 m_index = kInvalidIndex;
    u32 m_generation = 0;
};

} // namespace fuse
