// PipelineBatch (include/fuse/renderer/vk/pipeline_batch.hpp): builds run at once without a batch, on workers
// with one; wait() / waitFor() / failed() / nesting. CPU only (the builds are plain callables).
#include <fuse/renderer/vk/pipeline_batch.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

int main() {
    using namespace fuse::renderer;
    int ownerA = 0;
    int ownerB = 0;

    // No batch: the build runs now and its result is returned.
    expectTrue(PipelineBatch::current() == nullptr, "no batch open at start");
    int ran = 0;
    expectTrue(buildPipeline(&ownerA, [&ran] { return ++ran == 1; }), "unbatched build returns its result (true)");
    expectTrue(ran == 1, "unbatched build ran before buildPipeline returned");
    expectTrue(!buildPipeline(&ownerA, [] { return false; }), "unbatched build returns its result (false)");
    waitPipelineBuilds(&ownerA); // no-op without a batch

    // A batch: builds are queued (buildPipeline returns true), run on other threads, and wait() joins them.
    {
        std::atomic<int> done{0};
        std::atomic<bool> offThread{false};
        const std::thread::id self = std::this_thread::get_id();
        PipelineBatch batch(4);
        expectTrue(PipelineBatch::current() == &batch, "batch is current on its thread");
        for (int i = 0; i < 16; ++i) {
            const bool queued = buildPipeline(
                &ownerA,
                [&done, &offThread, self] {
                    sleepMs(5);
                    if (std::this_thread::get_id() != self) {
                        offThread = true;
                    }
                    ++done;
                    return true;
                },
                static_cast<fuse::u64>(i));
            expectTrue(queued, "batched build reports queued");
        }
        expectTrue(batch.queued() == 16u, "16 builds queued");
        expectTrue(batch.wait(), "all builds succeeded");
        expectTrue(done == 16, "wait() returns after every build ran");
        expectTrue(offThread, "builds ran on worker threads");
        expectTrue(!batch.failed(&ownerA), "no failure recorded");

        // A failure is reported by wait() and keyed by owner.
        (void)buildPipeline(&ownerB, [] { return false; });
        (void)buildPipeline(&ownerA, [] { return true; });
        expectTrue(!batch.wait(), "wait() reports the failed build");
        expectTrue(batch.failed(&ownerB) && !batch.failed(&ownerA), "failed() names the failing owner only");

        // waitFor(owner): that owner's builds are finished on return, other owners' may still be pending.
        std::atomic<bool> aDone{false};
        std::atomic<bool> release{false};
        std::atomic<bool> bDone{false};
        (void)buildPipeline(&ownerB, [&release, &bDone] {
            while (!release) {
                sleepMs(1);
            }
            bDone = true;
            return true;
        }, 1000u);
        (void)buildPipeline(&ownerA, [&aDone] {
            sleepMs(10);
            aDone = true;
            return true;
        });
        waitPipelineBuilds(&ownerA);
        expectTrue(aDone, "waitFor(owner) returns after that owner's builds");
        expectTrue(!bDone, "waitFor(owner) does not wait for other owners");
        release = true;

        // A nested batch is inactive: its builds go to the outer batch.
        {
            PipelineBatch inner;
            expectTrue(PipelineBatch::current() == &batch, "the outer batch stays current");
            std::atomic<bool> innerDone{false};
            (void)buildPipeline(&ownerA, [&innerDone] {
                innerDone = true;
                return true;
            });
            (void)inner.wait();
            expectTrue(innerDone, "an inner batch's wait() waits for the outer batch's builds");
        }
        (void)batch.wait();
        expectTrue(bDone, "wait() covers the released build");
    }
    expectTrue(PipelineBatch::current() == nullptr, "batch closed at scope end");

    // A batch that is destroyed without wait() still runs every build first.
    std::atomic<int> late{0};
    {
        PipelineBatch batch(2);
        for (int i = 0; i < 6; ++i) {
            (void)buildPipeline(&ownerA, [&late] {
                sleepMs(2);
                ++late;
                return true;
            });
        }
    }
    expectTrue(late == 6, "the destructor waits for the queued builds");

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_pipeline_batch: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_pipeline_batch: OK\n");
    return EXIT_SUCCESS;
}
