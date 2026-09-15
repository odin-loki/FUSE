#pragma once

#include <fuse/handle.hpp>

#include <utility>
#include <vector>

namespace fuse {

/// Generation-checked slot map for long-lived objects (Track A P4 / B2.3 resources).
template <typename T>
class HandleMap {
public:
    Handle<T> insert(T&& value) {
        u32 index = 0;
        if (!m_freeList.empty()) {
            index = m_freeList.back();
            m_freeList.pop_back();
            Slot& slot = m_slots[index];
            slot.value = std::move(value);
            slot.occupied = true;
            ++slot.generation;
            if (slot.generation == 0) {
                slot.generation = 1;
            }
            return Handle<T>(index, slot.generation);
        }

        index = static_cast<u32>(m_slots.size());
        m_slots.push_back(Slot{std::move(value), 1, true});
        return Handle<T>(index, 1);
    }

    void remove(Handle<T> handle) {
        if (!valid(handle)) {
            return;
        }
        Slot& slot = m_slots[handle.index()];
        slot.occupied = false;
        slot.value = T{};
        m_freeList.push_back(handle.index());
    }

    T* get(Handle<T> handle) {
        if (!valid(handle)) {
            return nullptr;
        }
        return &m_slots[handle.index()].value;
    }

    const T* get(Handle<T> handle) const {
        if (!valid(handle)) {
            return nullptr;
        }
        return &m_slots[handle.index()].value;
    }

    bool valid(Handle<T> handle) const {
        if (!handle.isValid() || handle.index() >= m_slots.size()) {
            return false;
        }
        const Slot& slot = m_slots[handle.index()];
        return slot.occupied && slot.generation == handle.generation();
    }

    u32 size() const {
        u32 count = 0;
        for (const Slot& slot : m_slots) {
            if (slot.occupied) {
                ++count;
            }
        }
        return count;
    }

    template <typename Fn>
    void forEachOccupied(Fn&& fn) const {
        for (u32 i = 0; i < m_slots.size(); ++i) {
            const Slot& slot = m_slots[i];
            if (slot.occupied) {
                fn(Handle<T>(i, slot.generation));
            }
        }
    }

private:
    struct Slot {
        T value{};
        u32 generation = 0;
        bool occupied = false;
    };

    std::vector<Slot> m_slots;
    std::vector<u32> m_freeList;
};

} // namespace fuse
