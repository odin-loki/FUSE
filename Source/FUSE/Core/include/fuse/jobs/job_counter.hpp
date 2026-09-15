#pragma once

#include <fuse/types.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace fuse::jobs {

/// Dependency counter for fork-join jobs.
class JobCounter {
public:
    explicit JobCounter(u32 initial = 0);

    void reset(u32 value);
    void add(u32 delta);
    void signal();
    bool isComplete() const;
    u32 remaining() const;

    /// Block until remaining reaches zero (worker threads may continue scheduling).
    void wait();

private:
    std::atomic<u32> m_remaining;
    mutable std::mutex m_waitMutex;
    std::condition_variable m_waitCv;
};

} // namespace fuse::jobs
