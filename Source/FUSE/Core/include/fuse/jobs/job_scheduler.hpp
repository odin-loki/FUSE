#pragma once

#include <fuse/jobs/job_counter.hpp>
#include <fuse/types.hpp>

#include <functional>

namespace fuse::jobs {

/// Work-stealing thread pool with fiber-compatible API surface (U2).
/// OS-thread backed; cooperative fiber switch is deferred to a later WP-03 slice.
class JobScheduler {
public:
    using JobFn = std::function<void()>;

    static JobScheduler& instance();

    void initialize(u32 workerCount);
    void shutdown();

    void submit(JobFn job);

    u32 workerCount() const { return m_workerCount; }
    bool isSingleThreaded() const { return m_workerCount == 0; }
    bool isInitialized() const { return m_initialized; }

    /// Fork-join over [begin, end) with grain size (serial when single-threaded).
    template <typename Body>
    void parallel_for(u32 begin, u32 end, u32 grainSize, const Body& body);

private:
    JobScheduler();
    ~JobScheduler();

    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    struct Impl;
    Impl* m_impl = nullptr;

    u32 m_workerCount = 0;
    bool m_initialized = false;
};

template <typename Body>
void JobScheduler::parallel_for(u32 begin, u32 end, u32 grainSize, const Body& body) {
    if (grainSize == 0) {
        grainSize = 1;
    }

    if (!m_initialized || m_workerCount == 0 || begin >= end) {
        for (u32 i = begin; i < end; ++i) {
            body(i);
        }
        return;
    }

    JobCounter counter(0);
    for (u32 chunk = begin; chunk < end; chunk += grainSize) {
        const u32 chunkEnd = (chunk + grainSize < end) ? (chunk + grainSize) : end;
        counter.add(1);
        submit([chunk, chunkEnd, &body, &counter]() {
            for (u32 i = chunk; i < chunkEnd; ++i) {
                body(i);
            }
            counter.signal();
        });
    }
    counter.wait();
}

} // namespace fuse::jobs
