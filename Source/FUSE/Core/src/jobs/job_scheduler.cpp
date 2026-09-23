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
#include <type_traits>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace fuse::jobs {

namespace detail {

namespace {
thread_local WorkerState* g_workerState = nullptr;

/// Upper bound on job fibers per worker. Each parked wait holds one fiber; beyond this the worker
/// stops taking new jobs and only resumes parked ones (64 KiB stacks -> 8 MiB per worker max).
constexpr std::size_t kMaxFibersPerWorker = 128;

/// Job fibers each worker creates up front (16 x 64 KiB stacks = 1 MiB per worker on desktop).
/// Covers a fork-join nested inside up to ~15 blocked jobs per worker; deeper parking still grows
/// the pool on demand up to kMaxFibersPerWorker.
constexpr std::size_t kPrewarmedFibersPerWorker = 16;

/// How long an idle worker polls for new work before sleeping on the condition variable. A futex
/// wakeup costs tens of microseconds on virtualised hosts, so back-to-back fork-joins (a frame's
/// systems) find the workers still awake; an idle pool sleeps after this window.
constexpr std::chrono::microseconds kWorkerSpin{100};

/// How long a parallel_for caller polls for in-flight chunks before parking / sleeping.
constexpr std::chrono::microseconds kCallerSpin{200};

/// Safety net for a sleeping worker; wakeups are explicit (see JobScheduler::Impl::wakeSleepers).
constexpr std::chrono::milliseconds kWorkerSleep{10};

inline void cpuRelax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

/// Poll `ready` for up to `budget`, yielding the core between polls so an oversubscribed machine
/// still runs the thread being waited on. Returns true once `ready` holds.
template <typename Ready>
bool spinUntil(Ready&& ready, std::chrono::microseconds budget) {
    for (u32 i = 0; i < 32; ++i) {
        if (ready()) {
            return true;
        }
        cpuRelax();
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (u32 i = 1;; ++i) {
        if (ready()) {
            return true;
        }
        std::this_thread::yield();
        if ((i & 7u) == 0u && std::chrono::steady_clock::now() >= deadline) {
            return ready();
        }
    }
}
} // namespace

/// Pooled fork-join record for JobScheduler::parallel_for. The caller and up to one helper job per
/// worker claim chunks from `claim`, which packs a use generation with the number of unclaimed
/// chunks. Helpers carry the generation they were issued for, so a helper that only starts after
/// the caller returned (every chunk already claimed and done) sees a finished or newer generation
/// and leaves without touching the record's fields or the caller's stack. The caller therefore
/// returns as soon as its chunks complete and the record is recycled straight away.
struct ParallelForTask {
    static constexpr u32 kChunkBits = 24;
    static constexpr u64 kChunkMask = (u64{1} << kChunkBits) - 1u;
    /// Largest chunk count per dispatch; larger ranges widen the grain (bodies see the same indices).
    static constexpr u32 kMaxChunks = static_cast<u32>(kChunkMask);
    static constexpr u64 kGenerationMask = (u64{1} << (64u - kChunkBits)) - 1u;

    /// (generation << kChunkBits) | unclaimed chunks.
    std::atomic<u64> claim{0};
    std::atomic<u32> pendingChunks{0};
    u64 generation = 0;
    u32 begin = 0;
    u32 end = 0;
    u32 grainSize = 1;
    u32 chunkCount = 0;
    ParallelForRangeFn invoke = nullptr;
    const void* body = nullptr;
    std::mutex doneMutex;
    std::condition_variable doneCv;
    ParallelForTask* nextFree = nullptr;
    /// The owning scheduler's count of helper jobs queued but not yet started.
    std::atomic<u32>* queuedHelpers = nullptr;

    /// Entry point of a helper job issued for use `gen`.
    void runHelper(u64 gen) {
        queuedHelpers->fetch_sub(1, std::memory_order_relaxed);
        runChunks(gen);
    }

    /// Claim and run chunks of use `gen` until none are left.
    void runChunks(u64 gen) {
        u64 word = claim.load(std::memory_order_acquire);
        for (;;) {
            const u64 unclaimed = word & kChunkMask;
            if ((word >> kChunkBits) != gen || unclaimed == 0u) {
                return;
            }
            // acquire on success: the fields below were published with this generation's word.
            if (!claim.compare_exchange_weak(word, word - 1u, std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }
            const u32 chunk = chunkCount - static_cast<u32>(unclaimed);
            const u64 chunkBegin = static_cast<u64>(begin) + static_cast<u64>(chunk) * grainSize;
            const u64 chunkEnd = chunkBegin + grainSize < end ? chunkBegin + grainSize : end;
            invoke(body, static_cast<u32>(chunkBegin), static_cast<u32>(chunkEnd));
            // acq_rel: the caller observing zero sees every write the chunk bodies made. The record
            // outlives this call (it is pooled), so notifying after a possible reuse is harmless.
            if (pendingChunks.fetch_sub(1, std::memory_order_acq_rel) == 1u) {
                // A waiting caller checks pendingChunks under doneMutex before sleeping.
                std::lock_guard<std::mutex> lock(doneMutex);
                doneCv.notify_all();
            }
            word = claim.load(std::memory_order_acquire);
        }
    }
};

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
    /// Parked until this word reads zero (a JobCounter or a parallel_for's pending chunks).
    const std::atomic<u32>* waitingOn = nullptr;
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
        return createFiber();
    }

    /// Create a new job fiber (not added to freeFibers). Null at the cap or on failure.
    JobFiber* createFiber() {
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
            if (fiber->waitingOn != nullptr && fiber->waitingOn->load(std::memory_order_acquire) != 0u) {
                continue;
            }
            parked[i] = parked.back();
            parked.pop_back();
            return switchTo(fiber) ? 2 : 1;
        }
        return 0;
    }

    /// Called on a job fiber: park until the scheduler fiber sees `*word` reach zero.
    void yieldOnZero(const std::atomic<u32>* word) {
        JobFiber* self = current;
        self->waitingOn = word;
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

bool workerWaitOnZero(const std::atomic<u32>* word) {
    WorkerState* state = g_workerState;
    if (!state || !state->schedulerFiber || !state->current) {
        return false;
    }

    while (word->load(std::memory_order_acquire) != 0u) {
        state->yieldOnZero(word);
    }
    return true;
}

bool workerWaitOnCounter(JobCounter* counter) {
    return workerWaitOnZero(&counter->m_remaining);
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
    /// Jobs sitting in any queue. Raised (seq_cst) before a job is published; see `sleepers`.
    std::atomic<u32> queued{0};
    /// Workers blocked (or about to block) on waitCv. Paired with `queued` in a store-load
    /// handshake (both seq_cst): a pusher that reads zero here is guaranteed the sleeper sees its
    /// job, so the common submit path skips waitMutex and the futex entirely.
    std::atomic<u32> sleepers{0};
    std::atomic<bool> useFibers{false};
    std::atomic<u32> roundRobin{0};
    u32 workerCount = 0;

    /// parallel_for records, recycled through an intrusive free list (grows only during warm-up).
    std::mutex taskPoolMutex;
    detail::ParallelForTask* freeTasks = nullptr;
    std::vector<std::unique_ptr<detail::ParallelForTask>> tasks;
    /// parallel_for helper jobs queued but not yet started. Capped so a caller that outpaces busy or
    /// sleeping workers (it runs the chunks itself) cannot pile up no-op helpers and grow the queues.
    std::atomic<u32> queuedHelpers{0};

    void finishJob() {
        // Nothing blocks on activeJobs (drain() polls), so completion needs no wakeup.
        activeJobs.fetch_sub(1, std::memory_order_acq_rel);
    }

    bool hasQueuedWork() const {
        return stop.load(std::memory_order_acquire) || queued.load(std::memory_order_seq_cst) > 0u;
    }

    /// Wake up to `count` sleeping workers after `queued` was raised.
    void wakeSleepers(u32 count) {
        if (count == 0u) {
            return;
        }
        const u32 sleeping = sleepers.load(std::memory_order_seq_cst);
        if (sleeping == 0u) {
            return;
        }
        {
            // A sleeper holds waitMutex from registering in `sleepers` until it blocks, so once we
            // hold it here every registered sleeper is either blocked or will re-check `queued`.
            std::lock_guard<std::mutex> lock(waitMutex);
        }
        if (count >= sleeping) {
            waitCv.notify_all();
        } else {
            for (u32 i = 0; i < count; ++i) {
                waitCv.notify_one();
            }
        }
    }

    /// Returns true when woken for work, false on the safety-net timeout.
    bool sleepUntilWork() {
        std::unique_lock<std::mutex> lock(waitMutex);
        sleepers.fetch_add(1, std::memory_order_seq_cst);
        const bool woken = waitCv.wait_for(lock, detail::kWorkerSleep, [this] { return hasQueuedWork(); });
        sleepers.fetch_sub(1, std::memory_order_relaxed);
        return woken;
    }

    detail::ParallelForTask* createTask() {
        tasks.push_back(std::make_unique<detail::ParallelForTask>());
        tasks.back()->queuedHelpers = &queuedHelpers;
        return tasks.back().get();
    }

    /// Pre-size the record pool for the usual nesting depth (every worker plus outside callers
    /// running a nested parallel_for) so the first frames do not grow it.
    void reserveTasks(u32 count) {
        std::lock_guard<std::mutex> lock(taskPoolMutex);
        tasks.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            detail::ParallelForTask* task = createTask();
            task->nextFree = freeTasks;
            freeTasks = task;
        }
    }

    detail::ParallelForTask* acquireTask() {
        std::lock_guard<std::mutex> lock(taskPoolMutex);
        if (freeTasks != nullptr) {
            detail::ParallelForTask* task = freeTasks;
            freeTasks = task->nextFree;
            task->nextFree = nullptr;
            return task;
        }
        return createTask();
    }

    void releaseTask(detail::ParallelForTask* task) {
        std::lock_guard<std::mutex> lock(taskPoolMutex);
        task->nextFree = freeTasks;
        freeTasks = task;
    }

    void workerLoop(u32 index) {
        detail::WorkerState& state = *workerStates[index];
        detail::g_workerState = &state;

        bool fibersEnabled = useFibers.load(std::memory_order_acquire);
        if (fibersEnabled) {
            state.fiberStackBytes = platform::recommendedFiberStackBytes();
            // Bookkeeping never exceeds the fiber cap; reserving it keeps park/resume heap-free.
            state.fibers.reserve(detail::kMaxFibersPerWorker);
            state.freeFibers.reserve(detail::kMaxFibersPerWorker);
            state.parked.reserve(detail::kMaxFibersPerWorker);
            state.schedulerFiber.reset(platform::fiberAllocateContext());
            if (state.schedulerFiber) {
                platform::fiberCaptureCurrent(state.schedulerFiber.get());
            }
            // Probe one fiber up front; without it this worker runs jobs directly on its thread.
            detail::JobFiber* probe = state.schedulerFiber ? state.acquireFiber() : nullptr;
            if (probe) {
                state.freeFibers.push_back(probe);
                // Pre-grow the pool so ordinary fork-join nesting never creates a fiber (three heap
                // allocations) mid-frame. Park depth depends on timing: when a thread running an
                // in-flight chunk is preempted, its waiter parks and this worker starts another
                // blocking job on a fresh fiber, so warm-up alone cannot be trusted to reach it.
                while (state.fibers.size() < detail::kPrewarmedFibersPerWorker) {
                    detail::JobFiber* fiber = state.createFiber();
                    if (!fiber) {
                        break;
                    }
                    state.freeFibers.push_back(fiber);
                }
            } else {
                fibersEnabled = false;
            }
        }

        // Spin before sleeping only after real activity; a timed-out sleep goes straight back to
        // sleep so an idle pool costs a few wakeups per second, not a spin window per timeout.
        bool spinBeforeSleep = true;
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
                // Stay awake briefly: the next fork-join usually arrives within microseconds.
                if (spinBeforeSleep && detail::spinUntil([this] { return hasQueuedWork(); }, detail::kWorkerSpin)) {
                    continue;
                }
                spinBeforeSleep = sleepUntilWork();
                continue;
            }
            queued.fetch_sub(1, std::memory_order_acq_rel);
            spinBeforeSleep = true;

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
        // Count before publishing so a worker that pops the job never drives `queued` below zero.
        queued.fetch_add(1, std::memory_order_seq_cst);
        const u32 target = roundRobin.fetch_add(1, std::memory_order_relaxed) % workerCount;
        {
            std::lock_guard<std::mutex> lock(queueMutexes[target]);
            auto& dest = isUrgent(priority) ? queues[target].high : queues[target].normal;
            dest.pushBack(std::move(job));
        }
        wakeSleepers(1);
    }

    /// Reserve up to `wanted` helper slots under the queued-helper cap (2 per worker).
    u32 reserveHelpers(u32 wanted) {
        const u32 cap = 2u * workerCount;
        u32 current = queuedHelpers.load(std::memory_order_relaxed);
        for (;;) {
            const u32 available = current < cap ? cap - current : 0u;
            const u32 granted = wanted < available ? wanted : available;
            if (granted == 0u ||
                queuedHelpers.compare_exchange_weak(current, current + granted, std::memory_order_relaxed)) {
                return granted;
            }
        }
    }

    void parallelFor(u32 begin, u32 end, u32 grainSize, detail::ParallelForRangeFn invoke, const void* body) {
        const u64 range = static_cast<u64>(end) - begin;
        if ((range + grainSize - 1u) / grainSize > detail::ParallelForTask::kMaxChunks) {
            grainSize = static_cast<u32>((range + detail::ParallelForTask::kMaxChunks - 1u) /
                                         detail::ParallelForTask::kMaxChunks);
        }
        const u32 chunkCount = static_cast<u32>((range + grainSize - 1u) / grainSize);
        const u32 wanted = (chunkCount - 1u < workerCount) ? chunkCount - 1u : workerCount;
        const u32 helpers = reserveHelpers(wanted);

        detail::ParallelForTask* task = acquireTask();
        // Wraps after 2^40 uses of one record; a helper would need to sit queued that long.
        task->generation = (task->generation + 1u) & detail::ParallelForTask::kGenerationMask;
        const u64 gen = task->generation;
        task->begin = begin;
        task->end = end;
        task->grainSize = grainSize;
        task->chunkCount = chunkCount;
        task->invoke = invoke;
        task->body = body;
        task->pendingChunks.store(chunkCount, std::memory_order_relaxed);
        task->claim.store((gen << detail::ParallelForTask::kChunkBits) | chunkCount, std::memory_order_release);

        // Helpers go to distinct queues so idle workers pick them up without stealing. The queue
        // mutex publishes the task fields above to whichever worker runs the helper.
        queued.fetch_add(helpers, std::memory_order_seq_cst);
        for (u32 h = 0; h < helpers; ++h) {
            auto helper = [task, gen]() { task->runHelper(gen); };
            static_assert(std::is_trivially_copyable<decltype(helper)>::value && sizeof(helper) <= 2 * sizeof(void*),
                          "parallel_for helper job must fit std::function's inline storage");
            const u32 target = roundRobin.fetch_add(1, std::memory_order_relaxed) % workerCount;
            std::lock_guard<std::mutex> lock(queueMutexes[target]);
            queues[target].high.pushBack(JobFn(helper));
        }
        wakeSleepers(helpers);

        // The caller works too: with every worker busy or asleep it simply runs all chunks itself,
        // so parallel_for never depends on a queued job being picked up.
        task->runChunks(gen);

        const auto chunksDone = [task] { return task->pendingChunks.load(std::memory_order_acquire) == 0u; };
        if (!chunksDone() && !detail::spinUntil(chunksDone, detail::kCallerSpin) &&
            !detail::workerWaitOnZero(&task->pendingChunks)) {
            std::unique_lock<std::mutex> lock(task->doneMutex);
            task->doneCv.wait(lock, chunksDone);
        }
        releaseTask(task);
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
    m_impl->reserveTasks(16u + 8u * workerCount);

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

void JobScheduler::parallelForDispatch(u32 begin, u32 end, u32 grainSize, detail::ParallelForRangeFn invoke,
                                       const void* body) {
    if (!m_impl) {
        invoke(body, begin, end);
        return;
    }
    m_impl->parallelFor(begin, end, grainSize, invoke, body);
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
