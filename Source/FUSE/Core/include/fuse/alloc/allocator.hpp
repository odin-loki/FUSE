#pragma once

#include <fuse/alloc/alloc_stats.hpp>
#include <fuse/alloc/leak_detector.hpp>
#include <fuse/types.hpp>

#include <cstddef>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace fuse::alloc {

struct AllocInfo {
    usize size = 0;
    usize alignment = alignof(std::max_align_t);
    const char* tag = nullptr;
};

/// Common allocator surface for FUSE memory domains (B1.3 stub hierarchy).
class IAllocator {
public:
    virtual ~IAllocator() = default;

    virtual void* alloc(AllocInfo info) = 0;
    virtual void free(void* ptr, usize size) = 0;
    virtual void reset() = 0;

    virtual AllocStats stats() const = 0;
    virtual const char* name() const = 0;
};

/// Base alignment of every allocator arena. Offset-based alignment inside an arena only yields an
/// aligned address when the arena itself is at least this aligned, so requests up to this value
/// are honoured and larger (or non power-of-two) alignments fail the allocation.
inline constexpr usize kArenaAlignment = 256u;

namespace detail {

usize alignUp(usize value, usize alignment);

/// True for power-of-two alignments no larger than kArenaAlignment (0 is treated as 1).
inline bool isSupportedAlignment(usize alignment) {
    return alignment <= kArenaAlignment && (alignment & (alignment - 1u)) == 0u;
}

/// Zero-initialised byte arena backing store whose data() sits on a kArenaAlignment boundary.
/// (A std::vector with an aligned allocator value-initialises byte by byte through
/// allocator_traits::construct, which made multi-megabyte arenas very slow to create in
/// unoptimised builds; this zero-fills with memset.)
class ArenaBytes {
public:
    ArenaBytes() = default;
    explicit ArenaBytes(usize bytes) { resize(bytes); }
    ArenaBytes(const ArenaBytes& other) { assign(other.m_data, other.m_size); }
    ArenaBytes(ArenaBytes&& other) noexcept
        : m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0u)) {}
    ArenaBytes& operator=(const ArenaBytes& other) {
        if (this != &other) {
            assign(other.m_data, other.m_size);
        }
        return *this;
    }
    ArenaBytes& operator=(ArenaBytes&& other) noexcept {
        if (this != &other) {
            release();
            m_data = std::exchange(other.m_data, nullptr);
            m_size = std::exchange(other.m_size, 0u);
        }
        return *this;
    }
    ~ArenaBytes() { release(); }

    /// Reallocates to `bytes`, keeping the common prefix and zero-filling any growth.
    void resize(usize bytes) {
        if (bytes == m_size) {
            return;
        }
        u8* next = bytes > 0u ? static_cast<u8*>(::operator new(bytes, std::align_val_t{kArenaAlignment})) : nullptr;
        const usize keep = bytes < m_size ? bytes : m_size;
        if (keep > 0u) {
            std::memcpy(next, m_data, keep);
        }
        if (bytes > keep) {
            std::memset(next + keep, 0, bytes - keep);
        }
        release();
        m_data = next;
        m_size = bytes;
    }

    u8* data() { return m_data; }
    const u8* data() const { return m_data; }
    usize size() const { return m_size; }
    bool empty() const { return m_size == 0u; }

private:
    void assign(const u8* source, usize bytes) {
        release();
        resize(bytes);
        if (bytes > 0u) {
            std::memcpy(m_data, source, bytes);
        }
    }
    void release() {
        if (m_data != nullptr) {
            if constexpr (kLeakDetectorEnabled) {
                LeakDetector::releaseRange(m_data, m_size);
            }
            ::operator delete(m_data, std::align_val_t{kArenaAlignment});
        }
        m_data = nullptr;
        m_size = 0u;
    }

    u8* m_data = nullptr;
    usize m_size = 0u;
};

} // namespace detail

} // namespace fuse::alloc
