#include <fuse/platform/fiber.hpp>
#include <fuse/platform/fiber_slot.hpp>
#include <fuse/platform/fiber_stack.hpp>
#include <fuse/platform/guard_page.hpp>

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

void testFiberASanAnnotationsWithoutSanitizer() {
    // Address-sanitizer builds may link the real hooks. This check is the non-ASan case only.
#if defined(__SANITIZE_ADDRESS__)
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#else
    expectTrue(!fuse::platform::fiberASanAnnotationsActive(),
               "ASan fiber annotations are inactive without address sanitizer");
#endif
#else
    expectTrue(!fuse::platform::fiberASanAnnotationsActive(),
               "ASan fiber annotations are inactive without address sanitizer");
#endif
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

void testGuardPageOk() {
    expectTrue(fuse::platform::guard_page_ok(0), "guard page count 0 is in range");
    expectTrue(fuse::platform::guard_page_ok(3), "guard page count 3 is in range");
    expectTrue(!fuse::platform::guard_page_ok(-1), "guard page count -1 is out of range");
    expectTrue(!fuse::platform::guard_page_ok(4), "guard page count 4 is out of range");
}

void testFiberSlotOk() {
    expectTrue(fuse::platform::fiber_slot_ok(0), "slot 0 is in range");
    expectTrue(fuse::platform::fiber_slot_ok(63), "slot 63 is in range");
    expectTrue(!fuse::platform::fiber_slot_ok(-1), "slot -1 is out of range");
    expectTrue(!fuse::platform::fiber_slot_ok(64), "slot 64 is out of range");
}

void testFiberStackOk() {
    expectTrue(fuse::platform::fiber_stack_ok(16384), "16 KiB power-of-two stack is accepted");
    expectTrue(fuse::platform::fiber_stack_ok(65536), "64 KiB power-of-two stack is accepted");
    expectTrue(!fuse::platform::fiber_stack_ok(8192), "8 KiB stack is below the minimum");
    expectTrue(!fuse::platform::fiber_stack_ok(24576), "a non-power-of-two stack is rejected");
}

} // namespace

int main() {
    testGuardPageOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core fiber tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testFiberSlotOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core fiber tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testFiberStackOk();
    testFiberBackendName();
    testFiberASanAnnotationsWithoutSanitizer();
    testFiberSwapRoundTrip();

    if (g_failures == 0) {
        std::printf("fuse_core fiber tests: all checks passed (backend=%s asan_fiber_annotations=%d)\n",
                    fuse::platform::fiberBackendName(),
                    fuse::platform::fiberASanAnnotationsActive() ? 1 : 0);
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core fiber tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
