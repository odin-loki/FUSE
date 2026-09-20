#include <fuse/log/logger.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool snapshotContains(
    const fuse::log::RecordSnapshot& snap,
    fuse::log::Level level,
    fuse::log::Channel channel,
    const char* needle) {
    for (fuse::u32 i = 0; i < snap.count; ++i) {
        const fuse::log::Record& rec = snap.records[i];
        if (rec.level == level && rec.channel == channel && std::strstr(rec.message, needle) != nullptr) {
            return true;
        }
    }
    return false;
}

struct LogCapture {
    std::string lastMessage;
    fuse::log::Level lastLevel = fuse::log::Level::Trace;
    int count = 0;

    static void sink(fuse::log::Level level, const char* message, void* userData) {
        auto* capture = static_cast<LogCapture*>(userData);
        capture->lastLevel = level;
        capture->lastMessage = message ? message : "";
        ++capture->count;
    }
};

void restoreLoggerDefaults() {
    auto& logger = fuse::log::Logger::instance();
    logger.setMinLevel(fuse::log::Level::Info);
    logger.setEnabledChannels(static_cast<fuse::u32>(fuse::log::Channel::All));
    logger.setSink(nullptr, nullptr);
}

void testSinkAndRingRecordInfoCore() {
    restoreLoggerDefaults();

    LogCapture capture;
    auto& logger = fuse::log::Logger::instance();
    logger.setSink(&LogCapture::sink, &capture);

    logger.log(fuse::log::Level::Info, fuse::log::Channel::Core, "hello %s", "core");

    expectTrue(capture.count == 1, "info core reaches sink");
    expectTrue(capture.lastMessage == "hello core", "sink captures formatted info core message");
    expectTrue(capture.lastLevel == fuse::log::Level::Info, "sink reports Info level");

    const fuse::log::RecordSnapshot snap = logger.snapshotRecords();
    expectTrue(snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Core, "hello core"),
               "ring contains Info Core message");
}

void testDisabledChannelDoesNotRecord() {
    restoreLoggerDefaults();

    LogCapture capture;
    auto& logger = fuse::log::Logger::instance();
    logger.setSink(&LogCapture::sink, &capture);
    logger.setEnabledChannels(static_cast<fuse::u32>(fuse::log::Channel::Core));
    expectTrue(logger.enabledChannels() == static_cast<fuse::u32>(fuse::log::Channel::Core),
               "enabledChannels reports Core-only mask");

    const int sinkBefore = capture.count;
    logger.log(fuse::log::Level::Info, fuse::log::Channel::Renderer, "renderer hidden %d", 7);

    expectTrue(capture.count == sinkBefore, "disabled channel does not reach sink");
    const fuse::log::RecordSnapshot snap = logger.snapshotRecords();
    expectTrue(!snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Renderer, "renderer hidden"),
               "disabled channel does not record");
}

void testMinLevelFiltersTrace() {
    restoreLoggerDefaults();

    LogCapture capture;
    auto& logger = fuse::log::Logger::instance();
    logger.setMinLevel(fuse::log::Level::Info);
    logger.setSink(&LogCapture::sink, &capture);

    const int sinkBefore = capture.count;
    logger.log(fuse::log::Level::Trace, fuse::log::Channel::Core, "trace hidden %s", "noise");

    expectTrue(capture.count == sinkBefore, "minLevel filters Trace from sink");
    const fuse::log::RecordSnapshot snap = logger.snapshotRecords();
    expectTrue(!snapshotContains(snap, fuse::log::Level::Trace, fuse::log::Channel::Core, "trace hidden"),
               "minLevel filters Trace from ring");
}

void testDefaultLogUsesCoreAndHelpers() {
    restoreLoggerDefaults();

    LogCapture capture;
    auto& logger = fuse::log::Logger::instance();
    logger.setMinLevel(fuse::log::Level::Debug);
    logger.setSink(&LogCapture::sink, &capture);

    logger.log(fuse::log::Level::Info, "plain %s", "core-default");
    fuse::log::debug("debug helper %s", "ok");
    fuse::log::error("error helper %s", "ok");
    fuse::log::fatal("fatal helper %s", "ok");

    const fuse::log::RecordSnapshot snap = logger.snapshotRecords();
    expectTrue(snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Core, "plain core-default"),
               "log(Level, fmt) records Channel::Core");
    expectTrue(snapshotContains(snap, fuse::log::Level::Debug, fuse::log::Channel::Core, "debug helper ok"),
               "debug helper records Debug");
    expectTrue(snapshotContains(snap, fuse::log::Level::Error, fuse::log::Channel::Core, "error helper ok"),
               "error helper records Error");
    expectTrue(snapshotContains(snap, fuse::log::Level::Fatal, fuse::log::Channel::Core, "fatal helper ok"),
               "fatal helper records Fatal");
}

} // namespace

int main() {
    testSinkAndRingRecordInfoCore();
    testDisabledChannelDoesNotRecord();
    testMinLevelFiltersTrace();
    testDefaultLogUsesCoreAndHelpers();
    restoreLoggerDefaults();

    if (g_failures == 0) {
        std::printf("fuse_core logger tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core logger tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
