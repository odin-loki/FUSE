#include <fuse/profiler/profiler.hpp>

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
    expectTrue(preflight.canExport(), "enabled profiler canExport per preflight");

    fuse::profiler::beginFrame();
    {
        FUSE_PROFILE_SCOPE("P2Gate");
    }
    fuse::profiler::endFrame();

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(isJsonObject(json), "enabled export is a JSON object");
    expectTrue(json.find("\"traceEvents\"") != std::string::npos, "enabled export has traceEvents");
    expectTrue(json.find("\"name\":\"P2Gate\"") != std::string::npos,
               "enabled export includes P2Gate scope name");
    expectTrue(hasOnlyValidChromePhaseTokens(json, true),
               "enabled export has valid chrome ph tokens including B and E");
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

} // namespace

int main() {
    testEnabledP2GateChromeTrace();
    testDisabledProfilerStillExportsJsonObject();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d chrome trace test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_chrome_trace_tests: all tests passed.\n");
    return EXIT_SUCCESS;
}
