#pragma once

#include <fuse/types.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace fuse::jobs {

namespace detail {
struct WorkerState;
} // namespace detail

/// Dependency counter for fork-join jobs.
class JobCounter {
public:
    explicit JobCounter(u32 initial = 0);

    void reset(u32 value);
    void add(u32 delta);
    void signal();
    bool isComplete() const;
    u32 remaining() const;

    /// Block until remaining reaches zero.
    /// On worker threads with cooperative fibers, yields the worker without blocking the OS thread.
    void wait();

private:
    friend struct detail::WorkerState;

    void registerFiberWaiter(detail::WorkerState* worker);
    void resumeFiberWaiters();

    std::atomic<u32> m_remaining;
    mutable std::mutex m_waitMutex;
    std::condition_variable m_waitCv;
    std::vector<detail::WorkerState*> m_fiberWaiters;
};

} // namespace fuse::jobs
