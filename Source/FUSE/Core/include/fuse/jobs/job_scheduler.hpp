#pragma once

#include <fuse/jobs/job_counter.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <memory>

namespace fuse::jobs {

namespace detail {
/// Runs `body(i)` for i in [begin, end); `body` is the type-erased parallel_for body.
using ParallelForRangeFn = void (*)(const void* body, u32 begin, u32 end);
} // namespace detail

enum class JobPriority : u8 { Low = 0, Normal = 1, High = 2, Critical = 3 };

/// Work-stealing scheduler with optional cooperative fibers on worker threads (WP-03).
/// OS threads back the pool; JobCounter::wait() yields on workers when fibers are available.
class JobScheduler {
public:
    using JobFn = std::function<void()>;

    static JobScheduler& instance();

    void initialize(u32 workerCount);
    void shutdown();

    /// Resize the pool. Drains in-flight work, then shutdown+initialize.
    /// Returns false if the scheduler is not initialized.
    bool setWorkerCount(u32 workerCount);

    void submit(JobFn job);
    void submit(JobFn job, JobPriority priority);

    u32 workerCount() const { return m_workerCount; }
    bool isSingleThreaded() const { return m_workerCount == 0; }
    bool isInitialized() const { return m_initialized; }

    /// Fork-join over [begin, end) with grain size (serial when single-threaded).
    /// Heap-free in steady state: the caller runs chunks itself alongside at most `workerCount()`
    /// helper jobs that claim chunks from a pooled dispatch record, and returns once every chunk
    /// has completed (without waiting for helpers that found nothing left to claim).
    template <typename Body>
    void parallel_for(u32 begin, u32 end, u32 grainSize, const Body& body);

private:
    JobScheduler();
    ~JobScheduler();

    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    void drainActiveJobs();

    void parallelForDispatch(u32 begin, u32 end, u32 grainSize, detail::ParallelForRangeFn invoke,
                             const void* body);

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    u32 m_workerCount = 0;
    bool m_initialized = false;
};

template <typename Body>
void JobScheduler::parallel_for(u32 begin, u32 end, u32 grainSize, const Body& body) {
    if (grainSize == 0) {
        grainSize = 1;
    }

    // One chunk or fewer gains nothing from dispatch; run it inline.
    if (!m_initialized || m_workerCount == 0 || begin >= end || end - begin <= grainSize) {
        for (u32 i = begin; i < end; ++i) {
            body(i);
        }
        return;
    }

    // The body stays on the caller's stack and is reached by pointer: dispatch blocks until every
    // chunk has run, and chunks are only ever claimed before that point.
    const detail::ParallelForRangeFn invoke = [](const void* bodyPtr, u32 chunkBegin, u32 chunkEnd) {
        const Body& typedBody = *static_cast<const Body*>(bodyPtr);
        for (u32 i = chunkBegin; i < chunkEnd; ++i) {
            typedBody(i);
        }
    };
    parallelForDispatch(begin, end, grainSize, invoke, &body);
}

} // namespace fuse::jobs
