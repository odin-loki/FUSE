#pragma once

#include <fuse/jobs/job_counter.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <memory>

namespace fuse::jobs {

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
    template <typename Body>
    void parallel_for(u32 begin, u32 end, u32 grainSize, const Body& body);

private:
    JobScheduler();
    ~JobScheduler();

    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    void drainActiveJobs();

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

    if (!m_initialized || m_workerCount == 0 || begin >= end) {
        for (u32 i = begin; i < end; ++i) {
            body(i);
        }
        return;
    }

    struct ParallelForState {
        JobCounter counter{0};
    };
    auto state = std::make_shared<ParallelForState>();
    for (u32 chunk = begin; chunk < end; chunk += grainSize) {
        const u32 chunkEnd = (chunk + grainSize < end) ? (chunk + grainSize) : end;
        state->counter.add(1);
        submit([state, chunk, chunkEnd, body]() {
            for (u32 i = chunk; i < chunkEnd; ++i) {
                body(i);
            }
            state->counter.signal();
        });
    }
    // The counter covers every chunk and each chunk owns its captures (shared state + body copy),
    // so nothing else needs to finish. Draining all active jobs here deadlocked when parallel_for
    // ran inside a job whose chunks completed before wait(): the caller counted itself as active.
    state->counter.wait();
}

} // namespace fuse::jobs
