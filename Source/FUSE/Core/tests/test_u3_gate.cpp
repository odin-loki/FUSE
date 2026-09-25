#include <fuse/core/temp_path.hpp>
#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/log/logger.hpp>

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

struct LogCapture {
    std::vector<std::string> messages;

    static void sink(fuse::log::Level /*level*/, const char* message, void* userData) {
        static_cast<LogCapture*>(userData)->messages.emplace_back(message);
    }

    bool contains(const char* needle) const {
        for (const std::string& message : messages) {
            if (message.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

void testFuseLoggerSink() {
    LogCapture capture;
    fuse::log::Logger::instance().setMinLevel(fuse::log::Level::Info);
    fuse::log::Logger::instance().setSink(&LogCapture::sink, &capture);

    fuse::log::info("fuse u3 gate logger probe");

#if defined(FUSE_NO_LOGGING) && FUSE_NO_LOGGING
    expectTrue(capture.messages.empty(), "shipping: Info output never reaches the sink");
#else
    expectTrue(capture.contains("fuse u3 gate logger probe"), "FUSE logger routes through custom sink");
#endif

    fuse::log::Logger::instance().setSink(nullptr, nullptr);
}

void testVfsAsyncLoadCommitsHandle() {
    const std::string tempPath = fuse::test::tempPath("fuse_u3_gate_asset.bin");
    {
        std::ofstream out(tempPath, std::ios::binary);
        out << "fuse-u3-gate";
    }

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(1);

    auto& vfs = fuse::io::VirtualFileSystem::instance();
    vfs.mount(fuse::io::MountKind::Game, fuse::test::tempDir(), "/game");

    fuse::HandleTable<fuse::io::Asset> table;
    const fuse::io::LoadId loadId = vfs.submitLoadAsync("/game/fuse_u3_gate_asset.bin");
    expectTrue(loadId != 0u, "async load returns non-zero id");

    for (fuse::u32 spinGuard = 0u; spinGuard < 1'000'000u && vfs.completedLoadCount() == 0u; ++spinGuard) {
        std::this_thread::yield();
    }
    expectTrue(vfs.completedLoadCount() > 0u, "async load completes on I/O lane");

    expectTrue(vfs.drainCompletedLoads(table) == 1u, "drain moves completed load to pending publish");
    expectTrue(table.pendingPublishCount() == 1u, "pending publish queued before commit");

    std::vector<fuse::Handle<fuse::io::Asset>> committed;
    expectTrue(table.commit(&committed) == 1u, "game thread commits handle");
    expectTrue(table.valid(committed[0]), "committed handle is valid");

    const fuse::io::Asset* live = table.get(committed[0]);
    expectTrue(live != nullptr, "committed handle resolves on game thread");
    expectTrue(live->virtualPath == "/game/fuse_u3_gate_asset.bin", "virtual path preserved");
    expectTrue(live->bytes.size() == 12u, "payload bytes match file size");

    scheduler.shutdown();
}

} // namespace

int main() {
    testFuseLoggerSink();
    testVfsAsyncLoadCommitsHandle();

    if (g_failures == 0) {
        std::printf("fuse_core u3 gate tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core u3 gate tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
