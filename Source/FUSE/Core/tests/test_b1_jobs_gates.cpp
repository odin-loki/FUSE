// B1.5 / B1.8 job-system gates (FUSE_MASTER_PLAN "B1.8 — Phase 1 Deliverables & Test Suite"):
//   - Scheduler initialises N worker threads (verified via thread count query + N-way rendezvous)
//   - 1M independent jobs complete with correct results across all workers
//   - Dependency chain C -> B -> A always executes in order (any submission order)
//   - Fiber yield/resume: a job waiting on a counter resumes after the counter reaches zero
//   - No deadlock under any ordering of job submission (10k randomised DAG submissions)
//   - parallel_for of 1M items is identical to the serial loop
// Every multi-worker test runs under a watchdog so a deadlock fails the test instead of hanging CI.

#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/platform/fiber.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// Aborts the process with a message when a section runs longer than `seconds` (deadlock guard).
class Watchdog {
public:
    Watchdog(const char* label, int seconds) : m_label(label) {
        m_thread = std::thread([this, seconds] {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_cv.wait_for(lock, std::chrono::seconds(seconds), [this] { return m_done; })) {
                std::fprintf(stderr, "FAIL: watchdog expired in '%s' (deadlock)\n", m_label);
                std::fflush(stderr);
                std::_Exit(EXIT_FAILURE);
            }
        });
    }

    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_done = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }

private:
    const char* m_label;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_done = false;
    std::thread m_thread;
};

template <typename Body>
void withScheduler(fuse::u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

#if !FUSE_JOBS_SINGLE_THREAD

void testSchedulerInitialisesNWorkers() {
    Watchdog watchdog("scheduler initialises N workers", 60);
    for (fuse::u32 workers : {1u, 2u, 3u, 4u, 8u}) {
        withScheduler(workers, [&] {
            auto& sched = fuse::jobs::JobScheduler::instance();
            expectTrue(sched.workerCount() == workers, "workerCount() reports the requested N");
            expectTrue(!sched.isSingleThreaded(), "N > 0 is not single-threaded");

            // N-way rendezvous: every job blocks until all N are running at once, which is only
            // possible when N distinct OS threads exist. Record their ids.
            std::atomic<fuse::u32> arrived{0};
            std::mutex idsMutex;
            std::set<std::thread::id> ids;
            fuse::jobs::JobCounter done(workers);
            for (fuse::u32 i = 0; i < workers; ++i) {
                sched.submit([&] {
                    {
                        std::lock_guard<std::mutex> lock(idsMutex);
                        ids.insert(std::this_thread::get_id());
                    }
                    arrived.fetch_add(1u, std::memory_order_acq_rel);
                    while (arrived.load(std::memory_order_acquire) < workers) {
                        std::this_thread::yield();
                    }
                    done.signal();
                });
            }
            done.wait();
            expectTrue(ids.size() == workers, "N distinct worker threads ran concurrently");
            expectTrue(ids.count(std::this_thread::get_id()) == 0u, "workers are not the caller thread");
        });
    }
}

void testOneMillionIndependentJobs() {
    Watchdog watchdog("1M independent jobs", 300);
    constexpr fuse::u32 kJobs = 1'000'000u;
    std::vector<fuse::u32> results(kJobs, 0u);
    constexpr fuse::u32 kMaxWorkers = 64u;
    std::vector<std::atomic<fuse::u32>> perWorker(kMaxWorkers);
    for (auto& c : perWorker) {
        c.store(0u);
    }
    std::mutex idsMutex;
    std::vector<std::thread::id> ids;

    const auto start = std::chrono::steady_clock::now();
    withScheduler(4, [&] {
        auto& sched = fuse::jobs::JobScheduler::instance();
        fuse::jobs::JobCounter counter(kJobs);
        fuse::u32* out = results.data();
        for (fuse::u32 i = 0; i < kJobs; ++i) {
            sched.submit([out, i, &counter, &perWorker, &idsMutex, &ids] {
                out[i] = i * 2654435761u ^ (i >> 3);
                thread_local fuse::u32 slot = kMaxWorkers;
                if (slot == kMaxWorkers) {
                    std::lock_guard<std::mutex> lock(idsMutex);
                    slot = static_cast<fuse::u32>(ids.size());
                    ids.push_back(std::this_thread::get_id());
                }
                perWorker[slot % kMaxWorkers].fetch_add(1u, std::memory_order_relaxed);
                counter.signal();
            });
        }
        counter.wait();
    });
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    fuse::u32 wrong = 0;
    for (fuse::u32 i = 0; i < kJobs; ++i) {
        if (results[i] != (i * 2654435761u ^ (i >> 3))) {
            ++wrong;
        }
    }
    fuse::u32 total = 0;
    fuse::u32 participating = 0;
    for (auto& c : perWorker) {
        total += c.load();
        participating += c.load() > 0u ? 1u : 0u;
    }
    std::printf("  1M jobs: %.1f ms, %u workers participated, %u wrong\n", ms, participating, wrong);
    expectTrue(wrong == 0u, "1M independent jobs produce correct results");
    expectTrue(total == kJobs, "every job executed exactly once");
    expectTrue(participating >= 2u, "jobs spread across more than one worker");
}

void testDependencyChainAlwaysInOrder() {
    Watchdog watchdog("dependency chain C->B->A", 120);
    constexpr fuse::u32 kIterations = 2000u;
    std::mt19937 rng(0xB15u);
    fuse::u32 violations = 0;

    for (fuse::u32 workers : {1u, 2u, 4u}) {
        withScheduler(workers, [&] {
            auto& sched = fuse::jobs::JobScheduler::instance();
            for (fuse::u32 it = 0; it < kIterations; ++it) {
                fuse::jobs::JobCounter aDone(1);
                fuse::jobs::JobCounter bDone(1);
                fuse::jobs::JobCounter cDone(1);
                std::atomic<fuse::u32> seq{0};
                fuse::u32 aSeq = 0;
                fuse::u32 bSeq = 0;
                fuse::u32 cSeq = 0;

                auto jobA = [&] {
                    aSeq = seq.fetch_add(1u) + 1u;
                    aDone.signal();
                };
                auto jobB = [&] {
                    aDone.wait();
                    bSeq = seq.fetch_add(1u) + 1u;
                    bDone.signal();
                };
                auto jobC = [&] {
                    bDone.wait();
                    cSeq = seq.fetch_add(1u) + 1u;
                    cDone.signal();
                };

                // Uniformly random over all six submission orders (C,B,A is the adversarial one).
                switch (rng() % 6u) {
                case 0: sched.submit(jobA); sched.submit(jobB); sched.submit(jobC); break;
                case 1: sched.submit(jobA); sched.submit(jobC); sched.submit(jobB); break;
                case 2: sched.submit(jobB); sched.submit(jobA); sched.submit(jobC); break;
                case 3: sched.submit(jobB); sched.submit(jobC); sched.submit(jobA); break;
                case 4: sched.submit(jobC); sched.submit(jobA); sched.submit(jobB); break;
                default: sched.submit(jobC); sched.submit(jobB); sched.submit(jobA); break;
                }
                cDone.wait();
                if (!(aSeq == 1u && bSeq == 2u && cSeq == 3u)) {
                    ++violations;
                }
            }
        });
    }
    expectTrue(violations == 0u, "C depends on B depends on A always executes A, B, C");
}

void testFiberWaitResumesAfterCounterZero() {
    if (!fuse::platform::cooperativeFibersAvailable()) {
        std::printf("SKIP: cooperative fibers unavailable on this platform\n");
        return;
    }
    Watchdog watchdog("fiber yield/resume", 60);

    withScheduler(1, [&] {
        auto& sched = fuse::jobs::JobScheduler::instance();
        fuse::jobs::JobCounter gate(3);
        std::atomic<fuse::u32> signalsSeenByWaiter{0};
        std::atomic<bool> resumed{false};
        std::thread::id waiterThreadBefore;
        std::thread::id waiterThreadAfter;
        std::atomic<fuse::u32> signallersRan{0};

        // One worker only: the waiter must yield its fiber so the same OS thread can run the
        // signalling jobs; a blocking wait here would deadlock.
        sched.submit([&] {
            waiterThreadBefore = std::this_thread::get_id();
            gate.wait();
            signalsSeenByWaiter.store(signallersRan.load(std::memory_order_acquire));
            waiterThreadAfter = std::this_thread::get_id();
            resumed.store(true, std::memory_order_release);
        });
        for (int i = 0; i < 3; ++i) {
            sched.submit([&] {
                expectTrue(!resumed.load(std::memory_order_acquire), "waiter does not resume early");
                signallersRan.fetch_add(1u, std::memory_order_acq_rel);
                gate.signal();
            });
        }
        while (!resumed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        expectTrue(signalsSeenByWaiter.load() == 3u, "waiter resumed only after the counter reached zero");
        expectTrue(waiterThreadBefore == waiterThreadAfter, "fiber resumes on its owning worker");
    });
}

struct RandomDag {
    std::vector<std::vector<fuse::u32>> deps;
};

void testNoDeadlockRandomSubmission() {
    Watchdog watchdog("10k random submission orders", 600);
    constexpr fuse::u32 kTests = 10'000u;
    std::mt19937 rng(0xDEADB15u);
    fuse::u32 orderViolations = 0;
    fuse::u32 totalJobs = 0;
    const fuse::u32 workerCycle[] = {1u, 2u, 3u, 4u};
    constexpr fuse::u32 kTestsPerPool = 500u;

    const auto start = std::chrono::steady_clock::now();
    auto& sched = fuse::jobs::JobScheduler::instance();
    sched.shutdown();
    for (fuse::u32 test = 0; test < kTests; ++test) {
        if (test % kTestsPerPool == 0u) {
            sched.shutdown();
            sched.initialize(workerCycle[(test / kTestsPerPool) % 4u]);
        }

        const fuse::u32 n = 2u + rng() % 23u;
        RandomDag dag;
        dag.deps.resize(n);
        for (fuse::u32 j = 1; j < n; ++j) {
            const fuse::u32 depCount = rng() % std::min<fuse::u32>(j + 1u, 4u);
            for (fuse::u32 d = 0; d < depCount; ++d) {
                dag.deps[j].push_back(rng() % j);
            }
        }

        std::vector<std::unique_ptr<fuse::jobs::JobCounter>> done(n);
        for (auto& c : done) {
            c = std::make_unique<fuse::jobs::JobCounter>(1);
        }
        std::vector<fuse::u32> seqOf(n, 0u);
        std::atomic<fuse::u32> seq{0};

        std::vector<fuse::u32> order(n);
        for (fuse::u32 j = 0; j < n; ++j) {
            order[j] = j;
        }
        std::shuffle(order.begin(), order.end(), rng);

        const bool nestedSubmit = (rng() % 4u) == 0u;
        for (fuse::u32 j : order) {
            auto job = [&, j] {
                for (fuse::u32 dep : dag.deps[j]) {
                    done[dep]->wait();
                }
                seqOf[j] = seq.fetch_add(1u, std::memory_order_acq_rel) + 1u;
                done[j]->signal();
            };
            const auto priority = static_cast<fuse::jobs::JobPriority>(rng() % 4u);
            if (nestedSubmit && j % 2u == 0u) {
                // Submit from inside another job so waits nest across fibers as well.
                sched.submit([&sched, job, priority] { sched.submit(job, priority); });
            } else {
                sched.submit(job, priority);
            }
        }
        for (auto& c : done) {
            c->wait();
        }

        for (fuse::u32 j = 0; j < n; ++j) {
            for (fuse::u32 dep : dag.deps[j]) {
                if (!(seqOf[dep] < seqOf[j])) {
                    ++orderViolations;
                }
            }
        }
        totalJobs += n;
    }
    sched.shutdown();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("  10k random DAG submissions: %u jobs, %.1f ms, 0 deadlocks\n", totalJobs, ms);
    expectTrue(orderViolations == 0u, "random DAGs respect every dependency edge");
}

#endif // !FUSE_JOBS_SINGLE_THREAD

void testParallelForOneMillionMatchesSerial() {
    Watchdog watchdog("parallel_for 1M", 120);
    constexpr fuse::u32 kCount = 1'000'000u;
    auto kernel = [](fuse::u32 i) {
        const float x = static_cast<float>(i) * 0.001f;
        return std::sin(x) * std::sqrt(x + 1.f) + static_cast<float>(i % 17u);
    };

    std::vector<float> serial(kCount);
    for (fuse::u32 i = 0; i < kCount; ++i) {
        serial[i] = kernel(i);
    }

#if FUSE_JOBS_SINGLE_THREAD
    const fuse::u32 workerCounts[] = {0u};
#else
    const fuse::u32 workerCounts[] = {0u, 1u, 4u};
#endif
    for (fuse::u32 workers : workerCounts) {
        for (fuse::u32 grain : {1u, 64u, 4096u, kCount}) {
            std::vector<float> parallel(kCount, -1.f);
            withScheduler(workers, [&] {
                fuse::jobs::parallel_for(0u, kCount, grain, [&](fuse::u32 i) { parallel[i] = kernel(i); });
            });
            expectTrue(std::memcmp(parallel.data(), serial.data(), kCount * sizeof(float)) == 0,
                       "parallel_for of 1M items is bit-identical to the serial loop");
        }
    }
}

} // namespace

int main() {
#if !FUSE_JOBS_SINGLE_THREAD
    testSchedulerInitialisesNWorkers();
    testOneMillionIndependentJobs();
    testDependencyChainAlwaysInOrder();
    testFiberWaitResumesAfterCounterZero();
    testNoDeadlockRandomSubmission();
#endif
    testParallelForOneMillionMatchesSerial();

    if (g_failures == 0) {
        std::printf("fuse_core_b1_jobs_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core_b1_jobs_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
