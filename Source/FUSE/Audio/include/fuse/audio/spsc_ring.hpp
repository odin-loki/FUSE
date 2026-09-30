#pragma once

#include <fuse/types.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <type_traits>
#include <vector>

namespace fuse::audio {

/// Lock-free single-producer / single-consumer ring of trivially copyable elements.
///
/// One thread calls write(), one other thread calls read() / discard(); the capacity is fixed by
/// reset() (rounded up to a power of two) and nothing allocates afterwards. Indices are
/// free-running u64 counters, so full / empty never alias. The producer publishes with a release
/// store of the write index after copying; the consumer acquires it before copying out (and the
/// reverse for the read index), which is all the synchronisation the element data needs.
template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing holds trivially copyable elements");

public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /// Not thread safe: call while neither side is running.
    void reset(usize min_capacity) {
        usize cap = 1;
        while (cap < min_capacity) {
            cap <<= 1u;
        }
        m_data.assign(min_capacity == 0 ? 0 : cap, T{});
        m_mask = m_data.empty() ? 0 : cap - 1;
        m_write.store(0, std::memory_order_relaxed);
        m_read.store(0, std::memory_order_relaxed);
    }

    usize capacity() const { return m_data.size(); }

    /// Elements the consumer can read now (consumer side; exact for the consumer).
    usize read_available() const {
        const u64 w = m_write.load(std::memory_order_acquire);
        const u64 r = m_read.load(std::memory_order_relaxed);
        return static_cast<usize>(w - r);
    }

    /// Elements the producer can write now (producer side; exact for the producer).
    usize write_available() const {
        const u64 w = m_write.load(std::memory_order_relaxed);
        const u64 r = m_read.load(std::memory_order_acquire);
        return m_data.size() - static_cast<usize>(w - r);
    }

    /// Approximate fill level for observers on a third thread (monitoring only).
    usize size_approx() const {
        const u64 r = m_read.load(std::memory_order_acquire);
        const u64 w = m_write.load(std::memory_order_acquire);
        return w >= r ? static_cast<usize>(w - r) : 0;
    }

    /// All-or-nothing write of `count` elements. Returns false (writes nothing) when they don't fit.
    bool write(const T* src, usize count) {
        if (count == 0) {
            return true;
        }
        const u64 w = m_write.load(std::memory_order_relaxed);
        const u64 r = m_read.load(std::memory_order_acquire);
        if (m_data.size() - static_cast<usize>(w - r) < count) {
            return false;
        }
        const usize start = static_cast<usize>(w) & m_mask;
        const usize first = std::min(count, m_data.size() - start);
        std::memcpy(m_data.data() + start, src, first * sizeof(T));
        if (first < count) {
            std::memcpy(m_data.data(), src + first, (count - first) * sizeof(T));
        }
        m_write.store(w + count, std::memory_order_release);
        return true;
    }

    /// All-or-nothing read of `count` elements. Returns false (reads nothing) when fewer are queued.
    bool read(T* dst, usize count) {
        if (count == 0) {
            return true;
        }
        const u64 r = m_read.load(std::memory_order_relaxed);
        const u64 w = m_write.load(std::memory_order_acquire);
        if (static_cast<usize>(w - r) < count) {
            return false;
        }
        const usize start = static_cast<usize>(r) & m_mask;
        const usize first = std::min(count, m_data.size() - start);
        std::memcpy(dst, m_data.data() + start, first * sizeof(T));
        if (first < count) {
            std::memcpy(dst + first, m_data.data(), (count - first) * sizeof(T));
        }
        m_read.store(r + count, std::memory_order_release);
        return true;
    }

private:
    std::vector<T> m_data;
    usize m_mask = 0;
    std::atomic<u64> m_write{0};
    std::atomic<u64> m_read{0};
};

} // namespace fuse::audio
