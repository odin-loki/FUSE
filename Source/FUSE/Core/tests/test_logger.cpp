#include <fuse/log/level_rank.hpp>
#include <fuse/log/log_slot.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/log/rate_allow.hpp>
#include <fuse/math/weighted_pick.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// Shipping (FUSE_NO_LOGGING) drops every sub-Fatal record and compiles the FUSE_LOG_* macros to a
// discard: sub-Fatal messages must reach neither the sink nor the ring; Fatal still does.
#if defined(FUSE_NO_LOGGING) && FUSE_NO_LOGGING
constexpr bool kLoggingStripped = true;
#else
constexpr bool kLoggingStripped = false;
#endif

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

    const fuse::log::RecordSnapshot snap = logger.snapshotRecords();
    if constexpr (kLoggingStripped) {
        expectTrue(capture.count == 0, "shipping: info core never reaches sink");
        expectTrue(capture.lastMessage.empty(), "shipping: sink captures no info message");
        expectTrue(!snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Core, "hello core"),
                   "shipping: ring holds no Info Core message");
    } else {
        expectTrue(capture.count == 1, "info core reaches sink");
        expectTrue(capture.lastMessage == "hello core", "sink captures formatted info core message");
        expectTrue(capture.lastLevel == fuse::log::Level::Info, "sink reports Info level");
        expectTrue(snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Core, "hello core"),
                   "ring contains Info Core message");
    }
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
    // Fatal is never stripped, in any configuration.
    expectTrue(snapshotContains(snap, fuse::log::Level::Fatal, fuse::log::Channel::Core, "fatal helper ok"),
               "fatal helper records Fatal");
    if constexpr (kLoggingStripped) {
        expectTrue(!snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Core, "plain core-default"),
                   "shipping: log(Level, fmt) Info not recorded");
        expectTrue(!snapshotContains(snap, fuse::log::Level::Debug, fuse::log::Channel::Core, "debug helper ok"),
                   "shipping: debug helper not recorded");
        expectTrue(!snapshotContains(snap, fuse::log::Level::Error, fuse::log::Channel::Core, "error helper ok"),
                   "shipping: error helper not recorded");
        expectTrue(capture.count == 1 && capture.lastLevel == fuse::log::Level::Fatal,
                   "shipping: only the Fatal message reaches the sink");
    } else {
        expectTrue(snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Core, "plain core-default"),
                   "log(Level, fmt) records Channel::Core");
        expectTrue(snapshotContains(snap, fuse::log::Level::Debug, fuse::log::Channel::Core, "debug helper ok"),
                   "debug helper records Debug");
        expectTrue(snapshotContains(snap, fuse::log::Level::Error, fuse::log::Channel::Core, "error helper ok"),
                   "error helper records Error");
    }
}

int g_macroArgEvaluations = 0;

int countMacroArgEvaluation() {
    ++g_macroArgEvaluations;
    return g_macroArgEvaluations;
}

void testLogMacros() {
    restoreLoggerDefaults();

    LogCapture capture;
    auto& logger = fuse::log::Logger::instance();
    logger.setMinLevel(fuse::log::Level::Trace);
    logger.setSink(&LogCapture::sink, &capture);

    g_macroArgEvaluations = 0;
    FUSE_LOG_INFO("macro info %d", countMacroArgEvaluation());
    FUSE_LOG_WARN("macro warn %d", countMacroArgEvaluation());
    FUSE_LOG_ERROR("macro error %d", countMacroArgEvaluation());

    // Arguments are evaluated in every configuration (shipping keeps side effects, drops the record).
    expectTrue(g_macroArgEvaluations == 3, "log macro arguments are evaluated exactly once");

    const fuse::log::RecordSnapshot snap = logger.snapshotRecords();
    if constexpr (kLoggingStripped) {
        expectTrue(capture.count == 0, "shipping: FUSE_LOG_* macros never reach the sink");
        expectTrue(!snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Core, "macro info"),
                   "shipping: FUSE_LOG_INFO produces no record");
        expectTrue(!snapshotContains(snap, fuse::log::Level::Error, fuse::log::Channel::Core, "macro error"),
                   "shipping: FUSE_LOG_ERROR produces no record");
    } else {
        expectTrue(capture.count == 3, "FUSE_LOG_* macros reach the sink");
        expectTrue(snapshotContains(snap, fuse::log::Level::Info, fuse::log::Channel::Core, "macro info 1"),
                   "FUSE_LOG_INFO records Info Core");
        expectTrue(snapshotContains(snap, fuse::log::Level::Error, fuse::log::Channel::Core, "macro error 3"),
                   "FUSE_LOG_ERROR records Error Core");
    }
}

void testRateAllow() {
    expectTrue(fuse::log::rate_allow(0, 0), "zero hits within a zero cap is allowed");
    expectTrue(fuse::log::rate_allow(3, 3), "hits equal to the cap is allowed");
    expectTrue(!fuse::log::rate_allow(-1, 3), "negative hits are not allowed");
    expectTrue(!fuse::log::rate_allow(4, 3), "hits above the cap are not allowed");
}

void testLogSlotOk() {
    expectTrue(fuse::log::log_slot_ok(0), "slot 0 is in range");
    expectTrue(fuse::log::log_slot_ok(255), "slot 255 is in range");
    expectTrue(!fuse::log::log_slot_ok(-1), "slot -1 is out of range");
    expectTrue(!fuse::log::log_slot_ok(256), "slot 256 is out of range");
}

void testLogLevelRank() {
    expectTrue(fuse::log::log_level_rank("Info") == 2, "Info ranks at 2");
    expectTrue(fuse::log::log_level_rank("info") == -1, "level names are case-sensitive");
    expectTrue(fuse::log::log_level_rank("Fatal") == 5, "Fatal ranks at 5");
}

void testWeightedPick() {
    const fuse::f32 weights[] = {1.f, 3.f};
    const auto at0 = fuse::math::weighted_index(weights, 2, 0.f);
    const auto at02 = fuse::math::weighted_index(weights, 2, 0.2f);
    const auto at05 = fuse::math::weighted_index(weights, 2, 0.5f);
    const auto at1 = fuse::math::weighted_index(weights, 2, 1.f);
    expectTrue(at0.has_value() && *at0 == 0, "u=0 picks the first weight");
    expectTrue(at02.has_value() && *at02 == 0, "u=0.2 stays in the first weight");
    expectTrue(at05.has_value() && *at05 == 1, "u=0.5 crosses into the second weight");
    expectTrue(at1.has_value() && *at1 == 1, "u=1 lands on the last positive weight");
}

} // namespace

int main() {
    testRateAllow();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core logger tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testLogSlotOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core logger tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testLogLevelRank();
    testWeightedPick();
    testSinkAndRingRecordInfoCore();
    testDisabledChannelDoesNotRecord();
    testMinLevelFiltersTrace();
    testDefaultLogUsesCoreAndHelpers();
    testLogMacros();
    restoreLoggerDefaults();

    if (g_failures == 0) {
        std::printf("fuse_core logger tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core logger tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
