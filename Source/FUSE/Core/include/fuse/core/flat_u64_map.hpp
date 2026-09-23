#pragma once

#include <fuse/types.hpp>

#include <utility>
#include <vector>

namespace fuse {

/// Open-addressing hash map from a 64-bit key to a small value, for per-frame lookup tables that
/// are cleared and refilled every step (contact pairs, entity slots). Unlike std::unordered_map,
/// clear() keeps all storage: once the table has grown to a frame's working size, refilling it
/// performs no heap allocation (FUSE_MASTER_PLAN B1.8). Iteration visits entries in insertion
/// order. Values should be cheap to copy; erase is not supported (clear and refill instead).
template <typename V>
class FlatU64Map {
public:
    [[nodiscard]] usize size() const { return m_order.size(); }
    [[nodiscard]] bool empty() const { return m_order.empty(); }

    void clear() {
        for (const u32 slot : m_order) {
            m_used[slot] = 0u;
        }
        m_order.clear();
    }

    void reserve(usize count) {
        if (count * 2u > m_used.size()) {
            rehash(capacityFor(count));
        }
        m_order.reserve(count);
    }

    /// Inserts `key -> value` unless present. Returns the stored value and whether it was inserted.
    std::pair<V*, bool> try_emplace(u64 key, const V& value) {
        if ((m_order.size() + 1u) * 2u > m_used.size()) {
            rehash(capacityFor(m_order.size() + 1u));
        }
        usize i = probeStart(key);
        while (m_used[i] != 0u) {
            if (m_keys[i] == key) {
                return {&m_values[i], false};
            }
            i = (i + 1u) & m_mask;
        }
        m_used[i] = 1u;
        m_keys[i] = key;
        m_values[i] = value;
        m_order.push_back(static_cast<u32>(i));
        return {&m_values[i], true};
    }

    V& operator[](u64 key) { return *try_emplace(key, V{}).first; }

    [[nodiscard]] V* find(u64 key) {
        const usize i = locate(key);
        return i == kNotFound ? nullptr : &m_values[i];
    }
    [[nodiscard]] const V* find(u64 key) const {
        const usize i = locate(key);
        return i == kNotFound ? nullptr : &m_values[i];
    }
    [[nodiscard]] bool contains(u64 key) const { return locate(key) != kNotFound; }

    /// `fn(u64 key, V& value)` for every entry, in insertion order.
    template <typename Fn>
    void for_each(Fn&& fn) {
        for (const u32 slot : m_order) {
            fn(m_keys[slot], m_values[slot]);
        }
    }
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const u32 slot : m_order) {
            fn(m_keys[slot], m_values[slot]);
        }
    }

    void swap(FlatU64Map& other) noexcept {
        m_keys.swap(other.m_keys);
        m_values.swap(other.m_values);
        m_used.swap(other.m_used);
        m_order.swap(other.m_order);
        std::swap(m_mask, other.m_mask);
    }

private:
    static constexpr usize kNotFound = ~static_cast<usize>(0);

    static usize capacityFor(usize count) {
        usize capacity = 16u;
        while (capacity < count * 2u) {
            capacity <<= 1u;
        }
        return capacity;
    }

    usize probeStart(u64 key) const {
        key ^= key >> 33u;
        key *= 0xff51afd7ed558ccdull;
        key ^= key >> 33u;
        return static_cast<usize>(key) & m_mask;
    }

    usize locate(u64 key) const {
        if (m_used.empty()) {
            return kNotFound;
        }
        for (usize i = probeStart(key);; i = (i + 1u) & m_mask) {
            if (m_used[i] == 0u) {
                return kNotFound;
            }
            if (m_keys[i] == key) {
                return i;
            }
        }
    }

    void rehash(usize capacity) {
        std::vector<u64> keys(capacity);
        std::vector<V> values(capacity);
        std::vector<u8> used(capacity, 0u);
        std::vector<u32> order;
        order.reserve(capacity / 2u);
        const usize mask = capacity - 1u;
        for (const u32 slot : m_order) {
            u64 h = m_keys[slot];
            h ^= h >> 33u;
            h *= 0xff51afd7ed558ccdull;
            h ^= h >> 33u;
            usize i = static_cast<usize>(h) & mask;
            while (used[i] != 0u) {
                i = (i + 1u) & mask;
            }
            used[i] = 1u;
            keys[i] = m_keys[slot];
            values[i] = std::move(m_values[slot]);
            order.push_back(static_cast<u32>(i));
        }
        m_keys.swap(keys);
        m_values.swap(values);
        m_used.swap(used);
        m_order.swap(order);
        m_mask = mask;
    }

    std::vector<u64> m_keys;
    std::vector<V> m_values;
    std::vector<u8> m_used;
    std::vector<u32> m_order;
    usize m_mask = 0;
};

} // namespace fuse
