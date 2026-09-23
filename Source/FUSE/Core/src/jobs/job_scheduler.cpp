#include <fuse/config.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/work_steal.hpp>
#include <fuse/jobs/worker_context.hpp>
#include <fuse/platform/fiber.hpp>
#include <fuse/platform/thread.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fuse::jobs {

namespace detail {

namespace {
thread_local WorkerState* g_workerState = nullptr;

/// Upper bound on job fibers per worker. Each parked wait holds one fiber; beyond this the worker
/// stops taking new jobs and only resumes parked ones (64 KiB stacks -> 8 MiB per worker max).
constexpr std::size_t kMaxFibersPerWorker = 128;
} // namespace

/// Growable FIFO ring. Steady-state push/pop never touches the heap (capacity only grows), which
/// keeps job submit -> execute allocation-free once the queues are warm.
class JobRing {
public:
    bool empty() const { return m_count == 0; }
    std::size_t size() const { return m_count; }

    void pushBack(JobScheduler::JobFn&& job) {
        if (m_count == m_slots.size()) {
            grow();
        }
        m_slots[(m_head + m_count) & (m_slots.size() - 1)] = std::move(job);
        ++m_count;
    }

    JobScheduler::JobFn popFront() {
        JobScheduler::JobFn job = std::move(m_slots[m_head]);
        m_slots[m_head] = nullptr;
        m_head = (m_head + 1) & (m_slots.size() - 1);
        --m_count;
        return job;
    }

    JobScheduler::JobFn popBack() {
        const std::size_t tail = (m_head + m_count - 1) & (m_slots.size() - 1);
        JobScheduler::JobFn job = std::move(m_slots[tail]);
        m_slots[tail] = nullptr;
        --m_count;
        return job;
    }

private:
    void grow() {
        const std::size_t newCapacity = m_slots.empty() ? 64u : m_slots.size() * 2u;
        std::vector<JobScheduler::JobFn> slots(newCapacity);
        for (std::size_t i = 0; i < m_count; ++i) {
            slots[i] = std::move(m_slots[(m_head + i) & (m_slots.size() - 1)]);
        }
        m_slots.swap(slots);
        m_head = 0;
    }

    std::vector<JobScheduler::JobFn> m_slots;
    std::size_t m_head = 0;
    std::size_t m_count = 0;
};

/// One cooperative job fiber. A fiber runs a job to completion or parks on a JobCounter; parked
/// fibers keep their stack while the worker runs other jobs on fresh fibers.
struct JobFiber {
    WorkerState* owner = nullptr;
    platform::UniqueFiber context;
    JobScheduler::JobFn job;
    JobCounter* waitingOn = nullptr;
    bool finished = false;
};

struct WorkerState {
    u32 index = 0;
    u32 fiberStackBytes = 0;
    platform::UniqueFiber schedulerFiber;
    JobFiber* current = nullptr;
    std::vector<std::unique_ptr<JobFiber>> fibers;
    std::vector<JobFiber*> freeFibers;
    std::vector<JobFiber*> parked;

    static void fiberEntry(void* arg) {
        auto* fiber = static_cast<JobFiber*>(arg);
        for (;;) {
            {
                JobScheduler::JobFn job = std::move(fiber->job);
                fiber->job = nullptr;
                if (job) {
                    job();
                }
            }
            fiber->finished = true;
            platform::fiberSwap(fiber->context.get(), fiber->owner->schedulerFiber.get());
        }
    }

    bool canAcquireFiber() const { return !freeFibers.empty() || fibers.size() < kMaxFibersPerWorker; }

    JobFiber* acquireFiber() {
        if (!freeFibers.empty()) {
            JobFiber* fiber = freeFibers.back();
            freeFibers.pop_back();
            return fiber;
        }
        if (fibers.size() >= kMaxFibersPerWorker) {
            return nullptr;
        }
        auto fiber = std::make_unique<JobFiber>();
        fiber->owner = this;
        fiber->context.reset(platform::fiberCreate(fiberStackBytes, fiberEntry, fiber.get()));
        if (!fiber->context) {
            return nullptr;
        }
        fibers.push_back(std::move(fiber));
        return fibers.back().get();
    }

    /// Switch into `fiber` until its job finishes (returns true) or it parks on a counter.
    bool switchTo(JobFiber* fiber) {
        current = fiber;
        fiber->finished = false;
        platform::fiberSwap(schedulerFiber.get(), fiber->context.get());
        current = nullptr;
        if (fiber->finished) {
            freeFibers.push_back(fiber);
            return true;
        }
        parked.push_back(fiber);
        return false;
    }

    /// Resume one parked fiber whose counter completed. Returns 0 when none was ready, 1 when a
    /// fiber resumed and parked again, 2 when a resumed fiber finished its job.
    int resumeReadyParked() {
        for (std::size_t i = 0; i < parked.size(); ++i) {
            JobFiber* fiber = parked[i];
            if (fiber->waitingOn != nullptr && !fiber->waitingOn->isComplete()) {
                continue;
            }
            parked[i] = parked.back();
            parked.pop_back();
            return switchTo(fiber) ? 2 : 1;
        }
        return 0;
    }

    /// Called on a job fiber: park until the scheduler fiber sees `counter` complete.
    void yieldOnCounter(JobCounter* counter) {
        JobFiber* self = current;
        self->waitingOn = counter;
        platform::fiberSwap(self->context.get(), schedulerFiber.get());
        self->waitingOn = nullptr;
    }

    void destroyFibers() {
        // Job fibers first: on Win32 releasing the scheduler fiber converts the thread back.
        freeFibers.clear();
        parked.clear();
        fibers.clear();
        schedulerFiber.reset();
    }
};

bool isWorkerThread() {
    return g_workerState != nullptr;
}

bool workerWaitOnCounter(JobCounter* counter) {
    WorkerState* state = g_workerState;
    if (!state || !state->schedulerFiber || !state->current) {
        return false;
    }

    while (!counter->isComplete()) {
        state->yieldOnCounter(counter);
    }
    return true;
}

WorkerState* currentWorkerState() {
    return g_workerState;
}

} // namespace detail

struct JobScheduler::Impl {
    struct WorkerQueues {
        detail::JobRing high;
        detail::JobRing normal;
    };

    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<detail::WorkerState>> workerStates;
    std::vector<WorkerQueues> queues;
    std::vector<std::mutex> queueMutexes;
    std::mutex waitMutex;
    std::condition_variable waitCv;
    std::atomic<bool> stop{false};
    std::atomic<u32> activeJobs{0};
    /// Jobs sitting in any queue. Incremented under waitMutex before notify so a worker that just
    /// found its queues empty cannot miss the wakeup and sleep out the 1 ms wait (lost-wakeup race).
    std::atomic<u32> queued{0};
    std::atomic<bool> useFibers{false};
    std::atomic<u32> roundRobin{0};
    u32 workerCount = 0;

    void finishJob() {
        activeJobs.fetch_sub(1, std::memory_order_acq_rel);
        waitCv.notify_all();
    }

    void workerLoop(u32 index) {
        detail::WorkerState& state = *workerStates[index];
        detail::g_workerState = &state;

        bool fibersEnabled = useFibers.load(std::memory_order_acquire);
        if (fibersEnabled) {
            state.fiberStackBytes = platform::recommendedFiberStackBytes();
            state.schedulerFiber.reset(platform::fiberAllocateContext());
            if (state.schedulerFiber) {
                platform::fiberCaptureCurrent(state.schedulerFiber.get());
            }
            // Probe one fiber up front; without it this worker runs jobs directly on its thread.
            detail::JobFiber* probe = state.schedulerFiber ? state.acquireFiber() : nullptr;
            if (probe) {
                state.freeFibers.push_back(probe);
            } else {
                fibersEnabled = false;
            }
        }

        while (!stop.load(std::memory_order_acquire)) {
            if (fibersEnabled && !state.parked.empty()) {
                const int resumed = state.resumeReadyParked();
                if (resumed == 2) {
                    finishJob();
                }
                if (resumed != 0) {
                    continue;
                }
            }

            JobFn job;
            const bool canStart = !fibersEnabled || state.canAcquireFiber();
            if (!canStart || (!tryPopLocal(index, job) && !trySteal(index, job))) {
                if (fibersEnabled && !state.parked.empty()) {
                    // Parked fibers wait on counters signalled by other workers; poll them.
                    std::this_thread::yield();
                    continue;
                }
                std::unique_lock<std::mutex> lock(waitMutex);
                waitCv.wait_for(lock, std::chrono::milliseconds(1), [this] {
                    return stop.load(std::memory_order_acquire) || queued.load(std::memory_order_acquire) > 0u;
                });
                continue;
            }
            queued.fetch_sub(1, std::memory_order_acq_rel);

            activeJobs.fetch_add(1, std::memory_order_acq_rel);
            detail::JobFiber* fiber = fibersEnabled ? state.acquireFiber() : nullptr;
            if (fiber) {
                fiber->job = std::move(job);
                if (state.switchTo(fiber)) {
                    finishJob();
                }
            } else {
                job();
                finishJob();
            }
        }

        state.destroyFibers();
        detail::g_workerState = nullptr;
    }

    static bool isUrgent(JobPriority priority) {
        return priority >= JobPriority::High;
    }

    bool tryPopLocal(u32 index, JobFn& out) {
        std::lock_guard<std::mutex> lock(queueMutexes[index]);
        auto& local = queues[index];
        if (!local.high.empty()) {
            out = local.high.popFront();
            return true;
        }
        if (!local.normal.empty()) {
            out = local.normal.popFront();
            return true;
        }
        return false;
    }

    bool tryStealFromBand(u32 thief, JobFn& out, bool highBand) {
        const u32 maxRounds = workerCount - 1;
        for (u32 round = 0; round < maxRounds; ++round) {
            const u32 victim = pickStealVictim(thief, workerCount, round);
            if (victim == thief) {
                continue;
            }

            // Lock both queues (deadlock-free ordering) and move the victim's newest half straight
            // into the thief's queue: no temporary batch storage, so stealing stays heap-free.
            std::scoped_lock lock(queueMutexes[victim], queueMutexes[thief]);
            auto& victimQueue = highBand ? queues[victim].high : queues[victim].normal;
            const std::size_t queueSize = victimQueue.size();
            if (!canStealFromVictim(queueSize)) {
                continue;
            }
            const u32 batch = stealHalfQueueBatchSize(queueSize);
            out = victimQueue.popBack();
            // Remaining batch items become local work so the thief does not re-steal
            // one-at-a-time from the same victim.
            auto& dest = highBand ? queues[thief].high : queues[thief].normal;
            for (u32 i = 1; i < batch && !victimQueue.empty(); ++i) {
                dest.pushBack(victimQueue.popBack());
            }
            return true;
        }
        return false;
    }

    bool trySteal(u32 thief, JobFn& out) {
        if (workerCount <= 1) {
            return false;
        }

        if (tryStealFromBand(thief, out, true)) {
            return true;
        }
        return tryStealFromBand(thief, out, false);
    }

    void pushJob(JobFn job, JobPriority priority) {
        const u32 target = roundRobin.fetch_add(1, std::memory_order_relaxed) % workerCount;
        {
            std::lock_guard<std::mutex> lock(queueMutexes[target]);
            auto& dest = isUrgent(priority) ? queues[target].high : queues[target].normal;
            dest.pushBack(std::move(job));
        }
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            queued.fetch_add(1, std::memory_order_acq_rel);
        }
        waitCv.notify_one();
    }

    void drain() {
        for (;;) {
            bool anyPending = false;
            for (u32 i = 0; i < workerCount; ++i) {
                std::lock_guard<std::mutex> lock(queueMutexes[i]);
                if (!queues[i].high.empty() || !queues[i].normal.empty()) {
                    anyPending = true;
                    break;
                }
            }
            if (!anyPending && activeJobs.load(std::memory_order_acquire) == 0) {
                break;
            }
            std::this_thread::yield();
        }
    }
};

JobScheduler& JobScheduler::instance() {
    static JobScheduler scheduler;
    return scheduler;
}

JobScheduler::JobScheduler() = default;

JobScheduler::~JobScheduler() {
    shutdown();
}

void JobScheduler::initialize(u32 workerCount) {
    if (m_initialized) {
        return;
    }

#if FUSE_JOBS_SINGLE_THREAD
    workerCount = 0;
#endif

    m_workerCount = workerCount;
    if (workerCount == 0) {
        m_initialized = true;
        return;
    }

    m_impl = std::make_unique<Impl>();
    m_impl->workerCount = workerCount;
    m_impl->queues.resize(workerCount);
    m_impl->queueMutexes = std::vector<std::mutex>(workerCount);
    m_impl->workerStates.resize(workerCount);
    m_impl->useFibers.store(platform::cooperativeFibersAvailable(), std::memory_order_release);

    for (u32 i = 0; i < workerCount; ++i) {
        m_impl->workerStates[i] = std::make_unique<detail::WorkerState>();
        m_impl->workerStates[i]->index = i;
    }

    for (u32 i = 0; i < workerCount; ++i) {
        m_impl->workers.emplace_back([this, i]() { m_impl->workerLoop(i); });
    }

    m_initialized = true;
}

void JobScheduler::shutdown() {
    if (!m_initialized) {
        return;
    }

    if (m_impl) {
        m_impl->stop.store(true, std::memory_order_release);
        m_impl->waitCv.notify_all();
        for (auto& worker : m_impl->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        m_impl.reset();
    }

    m_workerCount = 0;
    m_initialized = false;
}

bool JobScheduler::setWorkerCount(u32 workerCount) {
    if (!m_initialized) {
        return false;
    }

#if FUSE_JOBS_SINGLE_THREAD
    (void)workerCount;
    return true;
#else
    if (workerCount == m_workerCount) {
        return true;
    }

    drainActiveJobs();
    shutdown();
    initialize(workerCount);
    return true;
#endif
}

void JobScheduler::drainActiveJobs() {
    if (m_impl) {
        m_impl->drain();
    }
}

void JobScheduler::submit(JobFn job) {
    submit(std::move(job), JobPriority::Normal);
}

void JobScheduler::submit(JobFn job, JobPriority priority) {
    if (!m_initialized) {
        job();
        return;
    }

    if (m_workerCount == 0 || !m_impl) {
        job();
        return;
    }

    m_impl->pushJob(std::move(job), priority);
}

} // namespace fuse::jobs
