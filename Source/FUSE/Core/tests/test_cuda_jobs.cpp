#include <fuse/core/init.hpp>
#include <fuse/jobs/cuda_jobs.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

template <typename Body>
void withScheduler(fuse::u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

void testCudaAvailabilityMatchesBuild() {
#if defined(FUSE_HAS_CUDA)
    if (fuse::jobs::cudaJobsAvailable()) {
        std::printf("INFO: CUDA device available — runtime path active\n");
    } else {
        std::printf("SKIP: FUSE_HAS_CUDA but no CUDA device on this runner\n");
    }
#else
    expectTrue(!fuse::jobs::cudaJobsAvailable(), "stub build reports CUDA unavailable");
#endif
}

void testSubmitCudaSignalsCounter() {
    std::atomic<bool> launcherRan{false};

    withScheduler(2, [&] {
        fuse::jobs::JobCounter counter(1);
        fuse::jobs::CUDAJobDesc desc{};
        desc.counter = &counter;
        desc.tag = "test_stub";
        desc.kernel_launcher = [&](fuse::jobs::CUDAStreamHandle stream) {
            launcherRan.store(true, std::memory_order_release);
#if defined(FUSE_HAS_CUDA)
            if (fuse::jobs::cudaJobsAvailable()) {
                expectTrue(stream.native != nullptr, "CUDA path provides managed stream");
            } else {
                expectTrue(stream.native == nullptr, "no device keeps stream null");
            }
#else
            expectTrue(stream.native == nullptr, "stub path uses null stream handle");
#endif
        };

        fuse::jobs::submit_cuda(std::move(desc));
        counter.wait();
    });

    expectTrue(launcherRan.load(std::memory_order_acquire), "submit_cuda runs kernel launcher");
}

void testSubmitCudaWithoutScheduler() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();

    std::atomic<bool> ran{false};
    fuse::jobs::CUDAJobDesc desc{};
    desc.kernel_launcher = [&](fuse::jobs::CUDAStreamHandle) {
        ran.store(true, std::memory_order_release);
    };
    fuse::jobs::submit_cuda(std::move(desc));
    expectTrue(ran.load(std::memory_order_acquire), "submit_cuda runs inline when scheduler down");
}

} // namespace

int main() {
    fuse::core::initialize();

    testCudaAvailabilityMatchesBuild();
    testSubmitCudaSignalsCounter();
    testSubmitCudaWithoutScheduler();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_cuda_jobs: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_cuda_jobs: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
