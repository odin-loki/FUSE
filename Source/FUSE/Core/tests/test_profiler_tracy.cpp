// WP-0.6: FUSE profiler macros with and without the Tracy backend (cmake/FuseTracy.cmake).
//
// Built twice from this one source:
//   fuse_core_profiler_tracy_mode_tests  the configured mode (FUSE_TRACY=OFF by default)
//   fuse_core_profiler_tracy_on_tests    FUSE_TRACY=1 + fuse_profiler_tracy, against the same
//                                        fuse_core (proves the enabled build compiles and links)
// FUSE_TRACY_TEST_EXPECT_ON says which one this is.
//
// Compile-time part (the zero-overhead check): the stringized expansion of every profiler macro
// mentions Tracy exactly when the backend is on, and in the default build no Tracy header is
// reachable. Runtime part: the Chrome-trace ring records the same events in both modes (the Tracy
// mapping is additive), and in the enabled build the adapter answers (version pin, no server).
// fuse_core_profiler_tracy_zero_overhead additionally runs nm over fuse_core and this binary.
#include <fuse/profiler/profiler.hpp>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>

#if defined(FUSE_TRACY_TEST_EXPECT_ON) && FUSE_TRACY_TEST_EXPECT_ON
constexpr bool kExpectTracy = true;
#else
constexpr bool kExpectTracy = false;
#if defined(TRACY_ENABLE) || defined(__TRACYC_HPP__) || defined(FUSE_TRACY)
#error "FUSE_TRACY=OFF build: Tracy must not be enabled or reachable from <fuse/profiler/profiler.hpp>"
#endif
#endif

#if defined(FUSE_NO_PROFILER) && FUSE_NO_PROFILER
constexpr bool kMacrosRecord = false;
#else
constexpr bool kMacrosRecord = true;
#endif

static_assert(fuse::profiler::kTracyEnabled == (kExpectTracy && kMacrosRecord),
              "fuse::profiler::kTracyEnabled does not match the build's FUSE_TRACY mode");

#define FUSE_TRACY_TEST_STR2(...) #__VA_ARGS__
#define FUSE_TRACY_TEST_STR(...) FUSE_TRACY_TEST_STR2(__VA_ARGS__)

namespace {

constexpr bool mentionsTracy(std::string_view expansion) {
    return expansion.find("tracy") != std::string_view::npos;
}

constexpr std::string_view kScopeExpansion = FUSE_TRACY_TEST_STR(FUSE_PROFILE_SCOPE("zone"));
constexpr std::string_view kCounterExpansion = FUSE_TRACY_TEST_STR(FUSE_PROFILE_COUNTER("track", 1));
constexpr std::string_view kSnapshotExpansion =
    FUSE_TRACY_TEST_STR(FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("track", 1));
constexpr std::string_view kFrameMarkExpansion = FUSE_TRACY_TEST_STR(FUSE_PROFILE_FRAME_MARK());
constexpr std::string_view kFrameMarkNamedExpansion = FUSE_TRACY_TEST_STR(FUSE_PROFILE_FRAME_MARK_NAMED("f"));
constexpr std::string_view kGpuExpansion = FUSE_TRACY_TEST_STR(FUSE_PROFILE_GPU_BEGIN("g", nullptr));
constexpr std::string_view kFlowExpansion = FUSE_TRACY_TEST_STR(FUSE_PROFILE_ASYNC_FLOW_BEGIN("f", 1u));

constexpr bool kTracyOn = fuse::profiler::kTracyEnabled;
static_assert(mentionsTracy(kScopeExpansion) == kTracyOn, "FUSE_PROFILE_SCOPE expansion");
static_assert(mentionsTracy(kCounterExpansion) == kTracyOn, "FUSE_PROFILE_COUNTER expansion");
static_assert(mentionsTracy(kSnapshotExpansion) == kTracyOn, "FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME expansion");
static_assert(mentionsTracy(kFrameMarkExpansion) == kTracyOn, "FUSE_PROFILE_FRAME_MARK expansion");
static_assert(mentionsTracy(kFrameMarkNamedExpansion) == kTracyOn, "FUSE_PROFILE_FRAME_MARK_NAMED expansion");
// GPU markers and async flows stay Chrome-trace only (GPU zones come from the renderer GpuProfiler).
static_assert(!mentionsTracy(kGpuExpansion) && !mentionsTracy(kFlowExpansion), "GPU/flow macros stay Tracy-free");
// Disabled: the frame mark is a pure no-op expression.
static_assert(kTracyOn || kFrameMarkExpansion == "static_cast<void>(0)", "FUSE_PROFILE_FRAME_MARK is not a no-op");

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// One call site fed two different names: the second must still be recorded under its own name.
void dynamicScope(const char* name) {
    FUSE_PROFILE_SCOPE(name);
}

int sideEffects = 0;
const char* countedName() {
    ++sideEffects;
    return "counted";
}

void workload() {
    {
        FUSE_PROFILE_SCOPE("tracy.outer");
        {
            FUSE_PROFILE_SCOPE("tracy.inner");
            FUSE_PROFILE_COUNTER("tracy.counter.int", 7);
            FUSE_PROFILE_COUNTER("tracy.counter.float", 2.5);
            FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("tracy.counter.snapshot", 3u);
        }
        dynamicScope("tracy.dynamic.a");
        dynamicScope("tracy.dynamic.b");
        // Built at run time (a pointer no call site has seen); static storage per the ring contract.
        static char runtime[32];
        std::snprintf(runtime, sizeof(runtime), "tracy.runtime.%s", "name");
        dynamicScope(runtime);
        FUSE_PROFILE_SCOPE(countedName());
    }
    FUSE_PROFILE_FRAME_MARK();
    FUSE_PROFILE_FRAME_MARK_NAMED("tracy.frame.named");
}

} // namespace

int main() {
    fuse::profiler::reset();
    fuse::profiler::setEnabled(true);
    fuse::profiler::beginFrame();

    workload();
    std::thread worker([] { workload(); });
    worker.join();
    fuse::profiler::endFrame();

    if constexpr (kMacrosRecord) {
        // Begin + end per scope, per thread: the Tracy mapping must not change the ring's contents.
        expect(fuse::profiler::countEventsByName("tracy.outer") == 4u, "outer scope recorded in the ring");
        expect(fuse::profiler::countEventsByName("tracy.inner") == 4u, "inner scope recorded in the ring");
        expect(fuse::profiler::countEventsByName("tracy.dynamic.a") == 4u, "first dynamic name recorded");
        expect(fuse::profiler::countEventsByName("tracy.dynamic.b") == 4u, "second dynamic name recorded");
        expect(fuse::profiler::countEventsByName("tracy.runtime.name") == 4u, "runtime-built name recorded");
        expect(fuse::profiler::countEventsByName("tracy.counter.int") == 2u, "int counter recorded");
        expect(fuse::profiler::countEventsByName("tracy.counter.float") == 2u, "float counter recorded");
        expect(fuse::profiler::countEventsByName("tracy.counter.snapshot") == 2u, "snapshot counter recorded");
        expect(fuse::profiler::isScopeNestingBalanced(), "scope nesting balanced");
        // The scope name expression is evaluated exactly once per scope in both modes.
        expect(sideEffects == 2, "FUSE_PROFILE_SCOPE evaluates its name once");
    }

#if FUSE_PROFILER_TRACY_ACTIVE
    expect(std::strcmp(fuse::profiler::tracy_adapter::clientVersion(), "0.14.1") == 0,
           "tracy_adapter::clientVersion matches Engine/lib/tracy/VERSION");
    // TRACY_ON_DEMAND without a server: nothing connected, zones inactive, nothing buffered.
    expect(!fuse::profiler::tracy_adapter::connected(), "no Tracy server connected in the test");
    fuse::profiler::tracy_adapter::message("fuse_core_profiler_tracy_on");
    fuse::profiler::tracy_adapter::setThreadName("fuse-main");
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: profiler macros, Tracy backend %s\n", fuse::profiler::kTracyEnabled ? "ON" : "OFF");
    return 0;
}
