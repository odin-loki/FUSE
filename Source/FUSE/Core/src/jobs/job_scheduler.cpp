#include <fuse/config.hpp>
#include <fuse/jobs/job_scheduler.hpp>
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
    bool waitSatisfied = false;

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
        counter->registerFiberWaiter(this);
        platform::fiberSwap(jobFiber, schedulerFiber);
        waitingOn = nullptr;
    }

    void resumeCompletedWaits() {
        if (waitingOn && waitingOn->isComplete()) {
            waitSatisfied = true;
        }
    }

    void onCounterComplete(JobCounter* counter) {
        if (waitingOn == counter) {
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
    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<detail::WorkerState>> workerStates;
    std::vector<std::deque<JobFn>> queues;
    std::vector<std::mutex> queueMutexes;
    std::mutex waitMutex;
    std::condition_variable waitCv;
    std::atomic<bool> stop{false};
    std::atomic<u32> activeJobs{0};
    u32 workerCount = 0;
    bool useFibers = false;

    void workerLoop(u32 index) {
        detail::WorkerState& state = *workerStates[index];
        detail::g_workerState = &state;

        if (useFibers) {
            state.schedulerFiber = platform::fiberAllocateContext();
            platform::fiberCaptureCurrent(state.schedulerFiber);

            const u32 stackBytes = platform::recommendedFiberStackBytes();
            state.jobFiber = platform::fiberCreate(stackBytes, detail::WorkerState::jobFiberLoop, &state);
            if (!state.jobFiber) {
                useFibers = false;
            }
        }

        while (!stop.load(std::memory_order_acquire)) {
            if (useFibers && state.waitingOn && state.waitingOn->isComplete()) {
                state.waitSatisfied = true;
                platform::fiberSwap(state.schedulerFiber, state.jobFiber);
                continue;
            }

            if (useFibers && state.waitingOn && !state.waitingOn->isComplete()) {
                std::this_thread::yield();
                continue;
            }

            JobFn job;
            if (!tryPopLocal(index, job) && !trySteal(index, job)) {
                std::unique_lock<std::mutex> lock(waitMutex);
                waitCv.wait_for(lock, std::chrono::milliseconds(1), [this] {
                    return stop.load(std::memory_order_acquire);
                });
                continue;
            }

            activeJobs.fetch_add(1, std::memory_order_relaxed);
            if (useFibers && state.jobFiber) {
                state.runJob(std::move(job));
            } else {
                job();
            }
            activeJobs.fetch_sub(1, std::memory_order_relaxed);
            waitCv.notify_all();
        }

        if (useFibers) {
            platform::fiberDestroy(state.jobFiber);
            platform::fiberDestroy(state.schedulerFiber);
            state.jobFiber = nullptr;
            state.schedulerFiber = nullptr;
        }

        detail::g_workerState = nullptr;
    }

    bool tryPopLocal(u32 index, JobFn& out) {
        std::lock_guard<std::mutex> lock(queueMutexes[index]);
        if (queues[index].empty()) {
            return false;
        }
        out = std::move(queues[index].front());
        queues[index].pop_front();
        return true;
    }

    bool trySteal(u32 thief, JobFn& out) {
        for (u32 victim = 0; victim < workerCount; ++victim) {
            if (victim == thief) {
                continue;
            }
            std::lock_guard<std::mutex> lock(queueMutexes[victim]);
            if (queues[victim].empty()) {
                continue;
            }
            out = std::move(queues[victim].back());
            queues[victim].pop_back();
            return true;
        }
        return false;
    }

    void pushJob(JobFn job) {
        static std::atomic<u32> roundRobin{0};
        const u32 target = roundRobin.fetch_add(1, std::memory_order_relaxed) % workerCount;
        {
            std::lock_guard<std::mutex> lock(queueMutexes[target]);
            queues[target].push_back(std::move(job));
        }
        waitCv.notify_one();
    }

    void drain() {
        for (;;) {
            bool anyPending = false;
            for (u32 i = 0; i < workerCount; ++i) {
                std::lock_guard<std::mutex> lock(queueMutexes[i]);
                if (!queues[i].empty()) {
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

void JobCounter::registerFiberWaiter(detail::WorkerState* worker) {
    std::lock_guard<std::mutex> lock(m_waitMutex);
    m_fiberWaiters.push_back(worker);
}

void JobCounter::resumeFiberWaiters() {
    std::vector<detail::WorkerState*> waiters;
    {
        std::lock_guard<std::mutex> lock(m_waitMutex);
        waiters.swap(m_fiberWaiters);
    }
    for (detail::WorkerState* worker : waiters) {
        worker->onCounterComplete(this);
    }
}

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
    m_impl->useFibers = platform::cooperativeFibersAvailable();

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

void JobScheduler::submit(JobFn job) {
    if (!m_initialized) {
        job();
        return;
    }

    if (m_workerCount == 0 || !m_impl) {
        job();
        return;
    }

    m_impl->pushJob(std::move(job));
}

} // namespace fuse::jobs
