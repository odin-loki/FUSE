#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

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

} // namespace

int main() {
    testHandleTableDirectInsert();
    testHandleTableWorkerPublish();
    testVfsAsyncLoadRoundTrip();
    testVfsAsyncLoadResolveFailure();

    if (g_failures == 0) {
        std::printf("fuse_core io/handle tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core io/handle tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
