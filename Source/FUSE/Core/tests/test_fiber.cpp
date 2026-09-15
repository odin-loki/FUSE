#include <fuse/platform/fiber.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(const char* actual, const char* expected, const char* message) {
    if (std::strcmp(actual, expected) != 0) {
        std::fprintf(stderr, "FAIL: %s (expected \"%s\", got \"%s\")\n", message, expected, actual);
        ++g_failures;
    }
}

void testFiberBackendName() {
#if defined(_WIN32)
    expectEq(fuse::platform::fiberBackendName(), "win32", "Windows uses Win32 fiber backend");
#elif (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && \
    !(defined(FUSE_PLATFORM_MOBILE) && FUSE_PLATFORM_MOBILE)
    expectEq(fuse::platform::fiberBackendName(), "posix-ucontext", "desktop POSIX uses ucontext backend");
#else
    expectEq(fuse::platform::fiberBackendName(), "stub", "non-desktop targets use fiber stub");
    expectTrue(!fuse::platform::cooperativeFibersAvailable(), "stub backend reports fibers unavailable");
#endif
}

struct FiberSwapPayload {
    fuse::platform::FiberContext* mainFiber = nullptr;
    fuse::platform::FiberContext* secondaryFiber = nullptr;
    int phase = 0;
};

void secondaryFiberEntry(void* userData) {
    auto* payload = static_cast<FiberSwapPayload*>(userData);
    payload->phase = 1;
    fuse::platform::fiberSwap(payload->secondaryFiber, payload->mainFiber);
    payload->phase = 2;
}

void testFiberSwapRoundTrip() {
    if (!fuse::platform::cooperativeFibersAvailable()) {
        std::printf("SKIP: fiber swap round trip unavailable on this platform\n");
        return;
    }

    FiberSwapPayload payload;
    payload.mainFiber = fuse::platform::fiberAllocateContext();
    fuse::platform::fiberCaptureCurrent(payload.mainFiber);

    payload.secondaryFiber =
        fuse::platform::fiberCreate(4096u, secondaryFiberEntry, &payload);
    expectTrue(payload.secondaryFiber != nullptr, "secondary fiber allocates");

    fuse::platform::fiberSwap(payload.mainFiber, payload.secondaryFiber);
    expectTrue(payload.phase == 1, "main resumes after secondary fiber yields back");

    fuse::platform::fiberDestroy(payload.secondaryFiber);
    payload.secondaryFiber = nullptr;
    fuse::platform::fiberDestroy(payload.mainFiber);
    payload.mainFiber = nullptr;
}

} // namespace

int main() {
    testFiberBackendName();
    testFiberSwapRoundTrip();

    if (g_failures == 0) {
        std::printf("fuse_core fiber tests: all checks passed (backend=%s)\n",
                    fuse::platform::fiberBackendName());
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core fiber tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
