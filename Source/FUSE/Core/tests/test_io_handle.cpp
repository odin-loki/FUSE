#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
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

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
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

void testHandleTableDirectInsert() {
    fuse::HandleTable<fuse::io::Asset> table;
    fuse::io::Asset asset;
    asset.virtualPath = "/game/direct.bin";
    asset.bytes = {7u, 8u, 9u};

    const fuse::Handle<fuse::io::Asset> handle = table.insert(std::move(asset));
    expectTrue(table.valid(handle), "direct insert returns valid handle");

    const fuse::io::Asset* live = table.get(handle);
    expectTrue(live != nullptr, "direct insert is readable on game thread");
    expectTrue(live->bytes.size() == 3u, "direct insert preserves payload size");

    table.remove(handle);
    expectTrue(table.get(handle) == nullptr, "removed handle no longer resolves");
}

void testHandleTableWorkerPublish() {
    withScheduler(1, [] {
        fuse::HandleTable<fuse::io::Asset> table;
        fuse::jobs::JobCounter counter(1);

        fuse::jobs::JobScheduler::instance().submit([&]() {
            fuse::io::Asset asset;
            asset.virtualPath = "/game/worker.bin";
            asset.bytes = {1u, 2u, 3u};
            table.enqueuePublish(std::move(asset));
            counter.signal();
        });

        counter.wait();
        expectEq(table.pendingPublishCount(), 1u, "worker publish queues pending asset");

        std::vector<fuse::Handle<fuse::io::Asset>> committed;
        expectEq(table.commit(&committed), 1u, "game thread commit drains pending publishes");
        expectTrue(table.valid(committed[0]), "committed handle is valid");

        const fuse::io::Asset* live = table.get(committed[0]);
        expectTrue(live != nullptr && live->bytes.size() == 3u, "committed asset payload readable");
        expectEq(table.pendingPublishCount(), 0u, "pending queue empty after commit");
    });
}

void testVfsAsyncLoadRoundTrip() {
    const std::string tempPath = "/tmp/fuse_wp04_asset.bin";
    {
        std::ofstream out(tempPath, std::ios::binary);
        out << "fuse-wp04";
    }

    withScheduler(1, [&]() {
        auto& vfs = fuse::io::VirtualFileSystem::instance();
        vfs.mount(fuse::io::MountKind::Game, "/tmp", "/game");

        fuse::HandleTable<fuse::io::Asset> table;
        const fuse::io::LoadId loadId = vfs.submitLoadAsync("/game/fuse_wp04_asset.bin");
        expectTrue(loadId != 0u, "async load returns non-zero id");

        while (vfs.completedLoadCount() == 0u) {
            std::this_thread::yield();
        }

        expectEq(vfs.drainCompletedLoads(table), 1u, "completed load drains to handle table");
        expectEq(table.pendingPublishCount(), 1u, "drained load enqueues publish");

        std::vector<fuse::Handle<fuse::io::Asset>> committed;
        expectEq(table.commit(&committed), 1u, "game thread commits published asset");

        const fuse::io::Asset* live = table.get(committed[0]);
        expectTrue(live != nullptr, "async load handle resolves after commit");
        expectTrue(live->virtualPath == "/game/fuse_wp04_asset.bin", "async load preserves virtual path");
        expectTrue(live->bytes.size() == 9u, "async load reads full payload");
    });
}

void testVfsAsyncLoadResolveFailure() {
    withScheduler(0, []() {
        auto& vfs = fuse::io::VirtualFileSystem::instance();
        fuse::HandleTable<fuse::io::Asset> table;

        const fuse::io::LoadId loadId = vfs.submitLoadAsync("/unmounted/payload.bin");
        expectTrue(loadId != 0u, "failed resolve still returns load id");

        expectEq(vfs.drainCompletedLoads(table), 1u, "failed resolve still produces completion");
        expectEq(table.pendingPublishCount(), 0u, "failed resolve does not publish asset");
        expectTrue(!vfs.lastDrainedLoads().empty(), "failed resolve recorded in last drain batch");
        expectTrue(!vfs.lastDrainedLoads().front().success, "failed resolve marks success=false");
    });
}

void testHandleTablePendingNotLive() {
    withScheduler(1, [] {
        fuse::HandleTable<fuse::io::Asset> table;
        fuse::jobs::JobCounter counter(1);

        fuse::jobs::JobScheduler::instance().submit([&]() {
            fuse::io::Asset asset;
            asset.virtualPath = "/game/pending.bin";
            asset.bytes = {4u, 5u};
            table.enqueuePublish(std::move(asset));
            counter.signal();
        });

        counter.wait();
        expectEq(table.pendingPublishCount(), 1u, "pending publish queued");
        expectEq(table.liveCount(), 0u, "pending publish is not live before commit");
    });
}

void testHandleTableConcurrentWorkerPublish() {
    constexpr fuse::u32 kWorkers = 4u;
    constexpr fuse::u32 kPublishesPerWorker = 8u;

    withScheduler(kWorkers, [] {
        fuse::HandleTable<fuse::io::Asset> table;
        fuse::jobs::JobCounter counter(static_cast<fuse::u32>(kWorkers * kPublishesPerWorker));

        for (fuse::u32 worker = 0; worker < kWorkers; ++worker) {
            fuse::jobs::JobScheduler::instance().submit([&, worker]() {
                for (fuse::u32 item = 0; item < kPublishesPerWorker; ++item) {
                    fuse::io::Asset asset;
                    asset.virtualPath = "/game/w" + std::to_string(worker) + "_i" + std::to_string(item) + ".bin";
                    asset.bytes = {static_cast<fuse::u8>(worker), static_cast<fuse::u8>(item)};
                    table.enqueuePublish(std::move(asset));
                    counter.signal();
                }
            });
        }

        counter.wait();
        expectEq(table.pendingPublishCount(), kWorkers * kPublishesPerWorker,
                 "all concurrent publishes land in pending queue");

        std::vector<fuse::Handle<fuse::io::Asset>> committed;
        expectEq(table.commit(&committed), kWorkers * kPublishesPerWorker,
                 "single commit drains concurrent worker publishes");
        expectEq(table.liveCount(), kWorkers * kPublishesPerWorker, "committed handles are live");
        expectEq(table.pendingPublishCount(), 0u, "pending queue empty after commit");

        for (const fuse::Handle<fuse::io::Asset>& handle : committed) {
            expectTrue(table.valid(handle), "concurrent commit handle is valid");
            const fuse::io::Asset* live = table.get(handle);
            expectTrue(live != nullptr && live->bytes.size() == 2u, "concurrent commit payload readable");
        }
    });
}

void testHandleTableInterleavedPublishCommit() {
    constexpr fuse::u32 kWaves = 3u;
    constexpr fuse::u32 kPerWave = 4u;

    withScheduler(2, [] {
        fuse::HandleTable<fuse::io::Asset> table;
        std::atomic<fuse::u32> published{0u};

        for (fuse::u32 wave = 0; wave < kWaves; ++wave) {
            fuse::jobs::JobCounter counter(kPerWave);
            for (fuse::u32 item = 0; item < kPerWave; ++item) {
                fuse::jobs::JobScheduler::instance().submit([&, wave, item]() {
                    fuse::io::Asset asset;
                    asset.virtualPath = "/game/wave" + std::to_string(wave) + "_" + std::to_string(item) + ".bin";
                    asset.bytes = {static_cast<fuse::u8>(wave), static_cast<fuse::u8>(item)};
                    table.enqueuePublish(std::move(asset));
                    published.fetch_add(1u, std::memory_order_release);
                    counter.signal();
                });
            }

            counter.wait();
            expectEq(published.load(std::memory_order_acquire), (wave + 1u) * kPerWave,
                     "wave publishes complete before commit");

            std::vector<fuse::Handle<fuse::io::Asset>> committed;
            expectEq(table.commit(&committed), kPerWave, "game thread commits one wave at a time");
            expectEq(table.liveCount(), (wave + 1u) * kPerWave, "live count grows across waves");
            expectEq(table.pendingPublishCount(), 0u, "pending queue empty between waves");
        }
    });
}

void testHandleTableCommitWhileWorkersPublish() {
    constexpr fuse::u32 kTotal = 16u;

    withScheduler(2, [] {
        fuse::HandleTable<fuse::io::Asset> table;
        fuse::jobs::JobCounter counter(kTotal);
        std::atomic<fuse::u32> committedCount{0u};

        for (fuse::u32 item = 0; item < kTotal; ++item) {
            fuse::jobs::JobScheduler::instance().submit([&, item]() {
                fuse::io::Asset asset;
                asset.virtualPath = "/game/race_" + std::to_string(item) + ".bin";
                asset.bytes = {static_cast<fuse::u8>(item)};
                table.enqueuePublish(std::move(asset));
                counter.signal();
            });
        }

        while (committedCount.load(std::memory_order_acquire) < kTotal) {
            const fuse::u32 pending = table.pendingPublishCount();
            if (pending > 0u) {
                std::vector<fuse::Handle<fuse::io::Asset>> committed;
                const fuse::u32 batch = table.commit(&committed);
                committedCount.fetch_add(batch, std::memory_order_acq_rel);
            } else {
                std::this_thread::yield();
            }
        }

        counter.wait();
        expectEq(table.liveCount(), kTotal, "race commit loop installs every published asset");
        expectEq(table.pendingPublishCount(), 0u, "no pending publishes after race commit loop");
    });
}

void testVfsAsyncLoadCompletionOrdering() {
    const std::vector<std::string> assetNames = {
        "fuse_order_a.bin",
        "fuse_order_b.bin",
        "fuse_order_c.bin",
    };

    for (const std::string& name : assetNames) {
        const std::string tempPath = "/tmp/" + name;
        std::ofstream out(tempPath, std::ios::binary);
        out << name;
    }

    withScheduler(1, [&]() {
        auto& vfs = fuse::io::VirtualFileSystem::instance();
        vfs.mount(fuse::io::MountKind::Game, "/tmp", "/game");

        std::vector<fuse::io::LoadId> submitted;
        for (const std::string& name : assetNames) {
            const fuse::io::LoadId loadId = vfs.submitLoadAsync("/game/" + name);
            expectTrue(loadId != 0u, "ordered submit returns load id");
            submitted.push_back(loadId);
        }

        for (fuse::u32 spinGuard = 0u; spinGuard < 1'000'000u && vfs.completedLoadCount() < assetNames.size();
             ++spinGuard) {
            std::this_thread::yield();
        }

        expectEq(vfs.completedLoadCount(), static_cast<fuse::u32>(assetNames.size()),
                 "all ordered loads complete");

        const std::vector<fuse::io::LoadId> completionOrder = vfs.peekCompletedLoadOrder();
        expectEq(static_cast<fuse::u32>(completionOrder.size()), static_cast<fuse::u32>(assetNames.size()),
                 "peek returns every completed load id");

        for (std::size_t index = 1; index < completionOrder.size(); ++index) {
            expectTrue(completionOrder[index - 1] < completionOrder[index],
                       "completion ids stay in monotonic submission order");
            expectTrue(completionOrder[index - 1] == submitted[index - 1],
                       "peek order matches submission order");
        }

        fuse::HandleTable<fuse::io::Asset> table;
        expectEq(vfs.drainCompletedLoads(table), static_cast<fuse::u32>(assetNames.size()),
                 "ordered drain moves all completed loads");

        for (std::size_t index = 0; index < vfs.lastDrainedLoads().size(); ++index) {
            expectTrue(vfs.lastDrainedLoads()[index].id == submitted[index],
                       "drain batch preserves completion ordering");
        }

        std::vector<fuse::Handle<fuse::io::Asset>> committed;
        expectEq(table.commit(&committed), static_cast<fuse::u32>(assetNames.size()),
                 "game thread commits ordered drain batch");
    });
}

void testVfsStagedDrainPreservesCompletionOrder() {
    const std::vector<std::string> assetNames = {
        "fuse_partial_0.bin",
        "fuse_partial_1.bin",
        "fuse_partial_2.bin",
    };

    for (const std::string& name : assetNames) {
        const std::string tempPath = "/tmp/" + name;
        std::ofstream out(tempPath, std::ios::binary);
        out << name;
    }

    withScheduler(1, [&]() {
        auto& vfs = fuse::io::VirtualFileSystem::instance();
        vfs.mount(fuse::io::MountKind::Game, "/tmp", "/game");
        fuse::HandleTable<fuse::io::Asset> table;

        std::vector<fuse::io::LoadId> submitted;
        for (const std::string& name : assetNames) {
            submitted.push_back(vfs.submitLoadAsync("/game/" + name));

            for (fuse::u32 spinGuard = 0u; spinGuard < 1'000'000u && vfs.completedLoadCount() == 0u; ++spinGuard) {
                std::this_thread::yield();
            }

            expectEq(vfs.completedLoadCount(), 1u, "one completion visible before staged drain");
            const std::vector<fuse::io::LoadId> peekOrder = vfs.peekCompletedLoadOrder();
            expectEq(static_cast<fuse::u32>(peekOrder.size()), 1u, "peek sees single staged completion");
            expectTrue(peekOrder.front() == submitted.back(), "staged completion matches latest submit");

            expectEq(vfs.drainCompletedLoads(table), 1u, "staged drain moves one completion");
            expectEq(table.pendingPublishCount(), 1u, "each staged drain enqueues one publish");

            std::vector<fuse::Handle<fuse::io::Asset>> committed;
            expectEq(table.commit(&committed), 1u, "game thread commits staged drain");
            expectEq(table.pendingPublishCount(), 0u, "staged commit clears pending publish");
            expectTrue(vfs.lastDrainedLoads().front().id == submitted.back(),
                       "staged drain batch matches submission order");
        }

        expectEq(table.liveCount(), static_cast<fuse::u32>(assetNames.size()),
                 "staged commits install every async load");
        expectEq(vfs.completedLoadCount(), 0u, "no completions remain after staged drains");
    });
}

} // namespace

int main() {
    testHandleTableDirectInsert();
    testHandleTableWorkerPublish();
    testHandleTablePendingNotLive();
    testHandleTableConcurrentWorkerPublish();
    testHandleTableInterleavedPublishCommit();
    testHandleTableCommitWhileWorkersPublish();
    testVfsAsyncLoadRoundTrip();
    testVfsAsyncLoadResolveFailure();
    testVfsAsyncLoadCompletionOrdering();
    testVfsStagedDrainPreservesCompletionOrder();

    if (g_failures == 0) {
        std::printf("fuse_core io/handle tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core io/handle tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
