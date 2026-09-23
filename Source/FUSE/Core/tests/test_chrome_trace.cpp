#include <fuse/profiler/profiler.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

// Shipping (FUSE_NO_PROFILER) compiles profiler macros out and no-ops the chrome export: the
// export must still be a well-formed, empty trace.
#if defined(FUSE_NO_PROFILER) && FUSE_NO_PROFILER
constexpr bool kProfilerStripped = true;
#else
constexpr bool kProfilerStripped = false;
#endif

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool isJsonObject(const std::string& json) {
    return json.size() >= 2u && json.front() == '{' && json.back() == '}';
}

bool isValidChromePhaseToken(const std::string& json, std::size_t phPos) {
    static const char* kValid[] = {
        "\"ph\":\"B\"",
        "\"ph\":\"E\"",
        "\"ph\":\"s\"",
        "\"ph\":\"f\"",
        "\"ph\":\"C\"",
        "\"ph\":\"X\"",
    };
    for (const char* token : kValid) {
        if (json.compare(phPos, std::strlen(token), token) == 0) {
            return true;
        }
    }
    return false;
}

bool hasOnlyValidChromePhaseTokens(const std::string& json, bool requireBeginEnd) {
    bool sawBegin = false;
    bool sawEnd = false;
    std::size_t pos = 0;
    while ((pos = json.find("\"ph\":\"", pos)) != std::string::npos) {
        if (!isValidChromePhaseToken(json, pos)) {
            return false;
        }
        if (json.compare(pos, 8u, "\"ph\":\"B\"") == 0) {
            sawBegin = true;
        } else if (json.compare(pos, 8u, "\"ph\":\"E\"") == 0) {
            sawEnd = true;
        }
        pos += 6u;
    }
    return !requireBeginEnd || (sawBegin && sawEnd);
}

void testEnabledP2GateChromeTrace() {
    fuse::profiler::reset();
    fuse::profiler::setEnabled(true);

    const fuse::profiler::ChromeTraceExportPreflight preflight =
        fuse::profiler::preflightChromeTraceExport();
    if constexpr (kProfilerStripped) {
        expectTrue(!preflight.canExport() && preflight.profilerDisabled,
                   "shipping: preflight reports export disabled");
    } else {
        expectTrue(preflight.canExport(), "enabled profiler canExport per preflight");
    }

    fuse::profiler::beginFrame();
    {
        FUSE_PROFILE_SCOPE("P2Gate");
    }
    fuse::profiler::endFrame();

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(isJsonObject(json), "enabled export is a JSON object");
    expectTrue(json.find("\"traceEvents\"") != std::string::npos, "enabled export has traceEvents");
    if constexpr (kProfilerStripped) {
        expectTrue(fuse::profiler::eventCount() == 0u, "shipping: FUSE_PROFILE_SCOPE captures zero events");
        expectTrue(json.find("P2Gate") == std::string::npos, "shipping: export carries no scope name");
        expectTrue(json.find("\"ph\":") == std::string::npos, "shipping: export has no trace events");
    } else {
        expectTrue(json.find("\"name\":\"P2Gate\"") != std::string::npos,
                   "enabled export includes P2Gate scope name");
        expectTrue(hasOnlyValidChromePhaseTokens(json, true),
                   "enabled export has valid chrome ph tokens including B and E");
    }
}

void testDisabledProfilerStillExportsJsonObject() {
    fuse::profiler::reset();
    fuse::profiler::setEnabled(false);

    {
        FUSE_PROFILE_SCOPE("P2Gate");
    }

    const fuse::profiler::ChromeTraceExportPreflight preflight =
        fuse::profiler::preflightChromeTraceExport();
    expectTrue(preflight.profilerDisabled, "preflight marks profiler disabled");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(isJsonObject(json), "disabled profiler still exports a JSON object");
    expectTrue(json.find("\"traceEvents\"") != std::string::npos,
               "disabled export still declares traceEvents");
    expectTrue(hasOnlyValidChromePhaseTokens(json, false),
               "disabled export has no invalid ph tokens");
}

void testGpuCudaMarkersExportChromeCompleteEvents() {
    fuse::profiler::reset();
    fuse::profiler::setEnabled(true);

    void* cmdBuffer = nullptr;
    fuse::profiler::profile_gpu_begin("GpuMarker", cmdBuffer);
    fuse::profiler::profile_gpu_end(cmdBuffer);

    void* cudaStream = nullptr;
    fuse::profiler::profile_cuda_begin("CudaMarker", cudaStream);
    fuse::profiler::profile_cuda_end(cudaStream);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(isJsonObject(json), "gpu/cuda export is a JSON object");
    expectTrue(hasOnlyValidChromePhaseTokens(json, false),
               "gpu/cuda export has valid chrome ph tokens");
    if constexpr (kProfilerStripped) {
        expectTrue(json.find("\"traceEvents\":[]") != std::string::npos,
                   "shipping: chrome export is an empty trace");
        expectTrue(json.find("GpuMarker") == std::string::npos && json.find("CudaMarker") == std::string::npos,
                   "shipping: chrome export carries no marker names");
        return;
    }
    expectTrue(json.find("\"name\":\"GpuMarker\"") != std::string::npos,
               "enabled export includes GpuMarker name");
    expectTrue(json.find("\"name\":\"CudaMarker\"") != std::string::npos,
               "enabled export includes CudaMarker name");
    expectTrue(json.find("\"ph\":\"X\"") != std::string::npos,
               "gpu/cuda markers export chrome complete events");
    expectTrue(json.find("\"cat\":\"gpu\"") != std::string::npos,
               "gpu marker uses gpu chrome category");
    expectTrue(json.find("\"cat\":\"cuda\"") != std::string::npos,
               "cuda marker uses cuda chrome category");
}

} // namespace

int main() {
    testEnabledP2GateChromeTrace();
    testDisabledProfilerStillExportsJsonObject();
    testGpuCudaMarkersExportChromeCompleteEvents();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d chrome trace test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_chrome_trace_tests: all tests passed.\n");
    return EXIT_SUCCESS;
}
