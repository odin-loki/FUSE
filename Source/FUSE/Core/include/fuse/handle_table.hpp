#pragma once

#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>

#include <mutex>
#include <utility>
#include <vector>

namespace fuse {

/// Generation-checked live table with a worker → game publish queue (WP-04).
/// I/O jobs call enqueuePublish(); the game thread calls commit() to install handles.
template <typename T>
class HandleTable {
public:
    /// Game thread: insert directly into the live table.
    Handle<T> insert(T&& value) { return m_live.insert(std::move(value)); }

    /// Game thread: remove a live slot (generation bump on reuse).
    void remove(Handle<T> handle) { m_live.remove(handle); }

    /// Game thread: resolve a committed handle.
    T* get(Handle<T> handle) { return m_live.get(handle); }
    const T* get(Handle<T> handle) const { return m_live.get(handle); }

    bool valid(Handle<T> handle) const { return m_live.valid(handle); }
    u32 liveCount() const { return m_live.size(); }

    /// I/O workers: queue payload for the next game-thread commit.
    void enqueuePublish(T&& value) {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.push_back(std::move(value));
    }

    u32 pendingPublishCount() const {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        return static_cast<u32>(m_pending.size());
    }

    /// Game thread: move pending publishes into live slots.
    u32 commit(std::vector<Handle<T>>* outCommitted = nullptr) {
        std::vector<T> batch;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            batch.swap(m_pending);
        }

        if (outCommitted != nullptr) {
            outCommitted->clear();
            outCommitted->reserve(batch.size());
        }

        u32 committed = 0;
        for (T& value : batch) {
            Handle<T> handle = m_live.insert(std::move(value));
            ++committed;
            if (outCommitted != nullptr) {
                outCommitted->push_back(handle);
            }
        }
        return committed;
    }

private:
    HandleMap<T> m_live;
    mutable std::mutex m_pendingMutex;
    std::vector<T> m_pending;
};

} // namespace fuse
