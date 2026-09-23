#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/log/logger.hpp>

extern "C" void fuse_t2d_Con_execute(const char* script);
extern "C" void fuse_t3d_Con_execute(const char* script);

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

void testBothDimensionsLogViaFuseLogger() {
    LogCapture capture;
    fuse::log::Logger::instance().setMinLevel(fuse::log::Level::Info);
    fuse::log::Logger::instance().setSink(&LogCapture::sink, &capture);

    fuse_t3d_Con_execute("echo T3D dimension alive");
    fuse_t2d_Con_execute("echo T2D dimension alive");

#if defined(FUSE_NO_LOGGING) && FUSE_NO_LOGGING
    // Shipping strips sub-Fatal logging: the console shims still run, but nothing reaches the sink.
    expectTrue(capture.messages.empty(), "shipping: console Info output never reaches the sink");
#else
    expectTrue(capture.contains("[t3d] Con::execute"), "t3d console routes through FUSE logger");
    expectTrue(capture.contains("[t2d] Con::execute"), "t2d console routes through FUSE logger");
    expectTrue(capture.contains("T3D dimension alive"), "t3d console payload captured");
    expectTrue(capture.contains("T2D dimension alive"), "t2d console payload captured");
#endif

    fuse::log::Logger::instance().setSink(nullptr, nullptr);
}

void testVfsAsyncLoadCommitsHandle() {
    const std::string tempPath = "/tmp/fuse_u3_gate_asset.bin";
    {
        std::ofstream out(tempPath, std::ios::binary);
        out << "fuse-u3-gate";
    }

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(1);

    auto& vfs = fuse::io::VirtualFileSystem::instance();
    vfs.mount(fuse::io::MountKind::Game, "/tmp", "/game");

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
    testBothDimensionsLogViaFuseLogger();
    testVfsAsyncLoadCommitsHandle();

    if (g_failures == 0) {
        std::printf("fuse_core u3 gate tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core u3 gate tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
