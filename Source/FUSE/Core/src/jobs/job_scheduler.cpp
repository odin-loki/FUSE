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
} // namespace

struct WorkerState {
    u32 index = 0;
    platform::FiberContext* schedulerFiber = nullptr;
    platform::FiberContext* jobFiber = nullptr;
    JobScheduler::JobFn pendingJob;
    JobCounter* waitingOn = nullptr;
    bool waitSatisfied = false; // owned by this worker's scheduler/job fibers only

    static void jobFiberLoop(void* arg) {
        auto* self = static_cast<WorkerState*>(arg);
        for (;;) {
            if (self->pendingJob) {
                JobScheduler::JobFn job = std::move(self->pendingJob);
                self->pendingJob = nullptr;
                job();
            }
            platform::fiberSwap(self->jobFiber, self->schedulerFiber);
        }
    }

    void runJob(JobScheduler::JobFn job) {
        pendingJob = std::move(job);
        platform::fiberSwap(schedulerFiber, jobFiber);
        resumeCompletedWaits();
    }

    void yieldOnCounter(JobCounter* counter) {
        waitingOn = counter;
        waitSatisfied = false;
        platform::fiberSwap(jobFiber, schedulerFiber);
        waitingOn = nullptr;
    }

    void resumeCompletedWaits() {
        if (waitingOn && waitingOn->isComplete()) {
            waitSatisfied = true;
        }
    }

};

bool isWorkerThread() {
    return g_workerState != nullptr;
}

bool workerWaitOnCounter(JobCounter* counter) {
    if (!g_workerState || !platform::cooperativeFibersAvailable()) {
        return false;
    }

    while (!counter->isComplete()) {
        g_workerState->yieldOnCounter(counter);
        if (counter->isComplete()) {
            return true;
        }
    }
    return true;
}

WorkerState* currentWorkerState() {
    return g_workerState;
}

} // namespace detail

struct JobScheduler::Impl {
    struct WorkerQueues {
        std::deque<JobFn> high;
        std::deque<JobFn> normal;
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
    u32 workerCount = 0;

    void workerLoop(u32 index) {
        detail::WorkerState& state = *workerStates[index];
        detail::g_workerState = &state;

        const bool fibersRequested = useFibers.load(std::memory_order_acquire);
        if (fibersRequested) {
            state.schedulerFiber = platform::fiberAllocateContext();
            platform::fiberCaptureCurrent(state.schedulerFiber);

            const u32 stackBytes = platform::recommendedFiberStackBytes();
            state.jobFiber = platform::fiberCreate(stackBytes, detail::WorkerState::jobFiberLoop, &state);
            if (!state.jobFiber) {
                useFibers.store(false, std::memory_order_release);
            }
        }

        while (!stop.load(std::memory_order_acquire)) {
            const bool fibersEnabled = useFibers.load(std::memory_order_acquire);
            if (fibersEnabled && state.waitingOn && state.waitingOn->isComplete()) {
                state.waitSatisfied = true;
                platform::fiberSwap(state.schedulerFiber, state.jobFiber);
                continue;
            }

            if (fibersEnabled && state.waitingOn && !state.waitingOn->isComplete()) {
                std::this_thread::yield();
                continue;
            }

            JobFn job;
            if (!tryPopLocal(index, job) && !trySteal(index, job)) {
                std::unique_lock<std::mutex> lock(waitMutex);
                waitCv.wait_for(lock, std::chrono::milliseconds(1), [this] {
                    return stop.load(std::memory_order_acquire) || queued.load(std::memory_order_acquire) > 0u;
                });
                continue;
            }
            queued.fetch_sub(1, std::memory_order_acq_rel);

            activeJobs.fetch_add(1, std::memory_order_relaxed);
            if (fibersEnabled && state.jobFiber) {
                state.runJob(std::move(job));
            } else {
                job();
            }
            activeJobs.fetch_sub(1, std::memory_order_relaxed);
            waitCv.notify_all();
        }

        if (useFibers.load(std::memory_order_acquire)) {
            platform::fiberDestroy(state.jobFiber);
            platform::fiberDestroy(state.schedulerFiber);
            state.jobFiber = nullptr;
            state.schedulerFiber = nullptr;
        }

        detail::g_workerState = nullptr;
    }

    static bool isUrgent(JobPriority priority) {
        return priority >= JobPriority::High;
    }

    static bool stealHalfFromQueue(std::deque<JobFn>& victimQueue, std::vector<JobFn>& stolen) {
        const std::size_t queueSize = victimQueue.size();
        if (!canStealFromVictim(queueSize)) {
            return false;
        }

        const u32 batch = stealHalfQueueBatchSize(queueSize);
        stolen.reserve(batch);
        for (u32 i = 0; i < batch && !victimQueue.empty(); ++i) {
            stolen.push_back(std::move(victimQueue.back()));
            victimQueue.pop_back();
        }
        return !stolen.empty();
    }

    bool tryPopLocal(u32 index, JobFn& out) {
        std::lock_guard<std::mutex> lock(queueMutexes[index]);
        auto& local = queues[index];
        if (!local.high.empty()) {
            out = std::move(local.high.front());
            local.high.pop_front();
            return true;
        }
        if (!local.normal.empty()) {
            out = std::move(local.normal.front());
            local.normal.pop_front();
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

            std::vector<JobFn> stolen;
            {
                std::lock_guard<std::mutex> lock(queueMutexes[victim]);
                auto& victimQueue = highBand ? queues[victim].high : queues[victim].normal;
                if (!stealHalfFromQueue(victimQueue, stolen)) {
                    continue;
                }
            }

            // Run one stolen job now; remaining batch items become local work so
            // the thief does not re-steal one-at-a-time from the same victim.
            out = std::move(stolen[0]);
            if (stolen.size() > 1) {
                std::lock_guard<std::mutex> lock(queueMutexes[thief]);
                auto& dest = highBand ? queues[thief].high : queues[thief].normal;
                for (std::size_t i = 1; i < stolen.size(); ++i) {
                    dest.push_back(std::move(stolen[i]));
                }
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
        static std::atomic<u32> roundRobin{0};
        const u32 target = roundRobin.fetch_add(1, std::memory_order_relaxed) % workerCount;
        {
            std::lock_guard<std::mutex> lock(queueMutexes[target]);
            auto& dest = isUrgent(priority) ? queues[target].high : queues[target].normal;
            dest.push_back(std::move(job));
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

    m_impl = new Impl();
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
        delete m_impl;
        m_impl = nullptr;
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
