// WP-0.6 RenderDoc in-app capture gate (CPU only, stub-safe).
//
//   * No RenderDoc in the process: create() reports "unavailable" cleanly (NotInjected) and every
//     call is a no-op (false / 0); the frame hook does nothing.
//   * Disabled (FUSE_RENDERDOC=0), a missing library (LoadFailed) and a library without
//     RENDERDOC_GetAPI (NoEntryPoint) are also clean.
//   * Trigger parsing: --capture-frame N / --capture-frame=N / --capture-count N and the
//     FUSE_RENDERDOC_* environment.
//   * With FUSE_RP_FAKE_RENDERDOC (a counting RENDERDOC_GetAPI double): API 1.6.0 negotiated,
//     the frame hook captures exactly frames [N, N + count), titles the capture, passes the
//     VkInstance dispatch pointer, trigger/multi-frame/discard/path template reach the API.
#include <fuse/renderer/vk/renderdoc_capture.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace {

using namespace fuse::renderer;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

void expectInert(RenderDocCapture& capture, const char* what) {
    std::string message = std::string(what) + ": inert";
    expect(!capture.available(), message.c_str());
    expect(!capture.triggerCapture(), "triggerCapture is a no-op");
    expect(!capture.startFrameCapture(), "startFrameCapture is a no-op");
    expect(!capture.endFrameCapture(), "endFrameCapture is a no-op");
    expect(!capture.discardFrameCapture(), "discardFrameCapture is a no-op");
    expect(!capture.isFrameCapturing(), "isFrameCapturing false");
    expect(!capture.setCaptureTitle("x"), "setCaptureTitle is a no-op");
    expect(capture.numCaptures() == 0u, "numCaptures 0");
    capture.scheduleCapture(0, 2);
    for (fuse::u64 frame = 0; frame < 4; ++frame) {
        capture.onFrameBegin(frame);
        capture.onFrameEnd(frame);
    }
    expect(capture.hookCaptures() == 0u, "frame hook captures nothing");
    int major = -1, minor = -1, patch = -1;
    capture.apiVersion(major, minor, patch);
    expect(major == 0 && minor == 0 && patch == 0, "no API version");
    expect(std::strncmp(renderDocStatusName(capture.status()), "unavailable", 11) == 0 ||
               capture.status() == RenderDocStatus::Disabled,
           "status name says unavailable");
}

void testUnavailable() {
    setEnv("FUSE_RENDERDOC", nullptr);
    setEnv("FUSE_RENDERDOC_LIB", nullptr);
    auto capture = RenderDocCapture::create(RenderDocCapture::descFromEnvironment());
    expect(capture != nullptr, "create never fails");
#if defined(__APPLE__)
    expect(capture->status() == RenderDocStatus::UnsupportedPlatform, "Apple: unsupported platform");
#else
    expect(capture->status() == RenderDocStatus::NotInjected, "no RenderDoc: NotInjected");
#endif
    std::printf("no RenderDoc: %s (%s)\n", renderDocStatusName(capture->status()), capture->message().c_str());
    expectInert(*capture, "not injected");

    RenderDocCaptureDesc disabled{};
    disabled.enabled = false;
    auto off = RenderDocCapture::create(disabled);
    expect(off->status() == RenderDocStatus::Disabled, "enabled=false: Disabled");
    expectInert(*off, "disabled");

#if !defined(__APPLE__)
    RenderDocCaptureDesc missing{};
    missing.libraryPath = "/nonexistent/fuse/librenderdoc.so";
    auto failed = RenderDocCapture::create(missing);
    expect(failed->status() == RenderDocStatus::LoadFailed, "missing library: LoadFailed");
    expectInert(*failed, "load failed");
#endif
#if defined(__linux__)
    RenderDocCaptureDesc noEntry{};
    noEntry.libraryPath = "libc.so.6"; // loadable, no RENDERDOC_GetAPI
    auto wrong = RenderDocCapture::create(noEntry);
    expect(wrong->status() == RenderDocStatus::NoEntryPoint, "library without RENDERDOC_GetAPI: NoEntryPoint");
    expectInert(*wrong, "no entry point");
#endif
}

void testTriggers() {
    {
        const char* argv[] = {"app", "--capture-frame", "12", "--capture-count=3"};
        RenderDocCaptureDesc desc{};
        expect(RenderDocCapture::parseCommandLine(4, argv, desc), "--capture-frame N parsed");
        expect(desc.captureFrame == 12 && desc.captureFrameCount == 3u, "--capture-frame 12 --capture-count=3");
    }
    {
        const char* argv[] = {"app", "--other", "--capture-frame=7"};
        RenderDocCaptureDesc desc{};
        expect(RenderDocCapture::parseCommandLine(3, argv, desc) && desc.captureFrame == 7, "--capture-frame=7");
    }
    {
        const char* argv[] = {"app", "--capture-frame", "-1"};
        RenderDocCaptureDesc desc{};
        expect(!RenderDocCapture::parseCommandLine(3, argv, desc) && desc.captureFrame == -1, "negative frame rejected");
    }
    {
        const char* argv[] = {"app", "--capture-frame=12x"};
        RenderDocCaptureDesc desc{};
        expect(!RenderDocCapture::parseCommandLine(2, argv, desc) && desc.captureFrame == -1, "junk rejected");
    }
    {
        const char* argv[] = {"app", "--capture-frames=3", "--capture-frame"};
        RenderDocCaptureDesc desc{};
        expect(!RenderDocCapture::parseCommandLine(3, argv, desc) && desc.captureFrame == -1,
               "prefix-only option and a missing value are ignored");
    }
    setEnv("FUSE_RENDERDOC_CAPTURE_FRAME", "42");
    setEnv("FUSE_RENDERDOC_CAPTURE_COUNT", "2");
    setEnv("FUSE_RENDERDOC_CAPTURE_PATH", "captures/wp06");
    setEnv("FUSE_RENDERDOC", "0");
    RenderDocCaptureDesc env = RenderDocCapture::descFromEnvironment();
    expect(env.captureFrame == 42 && env.captureFrameCount == 2u, "FUSE_RENDERDOC_CAPTURE_FRAME / _COUNT");
    expect(env.captureFilePathTemplate != nullptr && std::strcmp(env.captureFilePathTemplate, "captures/wp06") == 0,
           "FUSE_RENDERDOC_CAPTURE_PATH");
    expect(!env.enabled, "FUSE_RENDERDOC=0 disables");
    setEnv("FUSE_RENDERDOC_CAPTURE_FRAME", "nope");
    setEnv("FUSE_RENDERDOC", nullptr);
    env = RenderDocCapture::descFromEnvironment();
    expect(env.captureFrame == -1 && env.enabled, "malformed FUSE_RENDERDOC_CAPTURE_FRAME ignored");
    setEnv("FUSE_RENDERDOC_CAPTURE_FRAME", nullptr);
    setEnv("FUSE_RENDERDOC_CAPTURE_COUNT", nullptr);
    setEnv("FUSE_RENDERDOC_CAPTURE_PATH", nullptr);
}

#if defined(FUSE_RP_FAKE_RENDERDOC)
struct FakeCounters {
    fuse::u32 getApiCalls;
    fuse::u32 triggerCapture;
    fuse::u32 triggerMultiFrames;
    fuse::u32 startCapture;
    fuse::u32 endCapture;
    fuse::u32 discardCapture;
    fuse::u32 setTitle;
    fuse::u32 setPathTemplate;
    void* lastDevice;
    char lastTitle[128];
    char lastPathTemplate[256];
};

FakeCounters* fakeCounters() {
#if defined(_WIN32)
    HMODULE module = GetModuleHandleA(FUSE_RP_FAKE_RENDERDOC);
    return module != nullptr ? reinterpret_cast<FakeCounters* (*)()>(
                                   reinterpret_cast<void*>(GetProcAddress(module, "fuse_fake_renderdoc_counters")))()
                             : nullptr;
#else
    void* library = dlopen(FUSE_RP_FAKE_RENDERDOC, RTLD_NOW | RTLD_NOLOAD);
    if (library == nullptr) {
        return nullptr;
    }
    auto fn = reinterpret_cast<FakeCounters* (*)()>(dlsym(library, "fuse_fake_renderdoc_counters"));
    dlclose(library); // drops the NOLOAD reference only
    return fn != nullptr ? fn() : nullptr;
#endif
}

void testFakeRenderDoc() {
    // A VkInstance-shaped object: RenderDoc's device pointer is its first pointer (dispatch table).
    void* dispatch = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234560));
    void* fakeInstance = &dispatch;

    RenderDocCaptureDesc desc{};
    desc.libraryPath = FUSE_RP_FAKE_RENDERDOC;
    desc.vkInstance = fakeInstance;
    desc.captureFrame = 3;
    desc.captureFrameCount = 2;
    desc.captureFilePathTemplate = "captures/fuse_wp06";
    auto capture = RenderDocCapture::create(desc);
    std::printf("fake RenderDoc: %s (%s)\n", renderDocStatusName(capture->status()), capture->message().c_str());
    expect(capture->available(), "fake RenderDoc: available");
    FakeCounters* counters = fakeCounters();
    expect(counters != nullptr, "fake RenderDoc counters reachable");
    if (!capture->available() || counters == nullptr) {
        return;
    }
    int major = 0, minor = 0, patch = 0;
    capture->apiVersion(major, minor, patch);
    expect(major == 1 && minor == 6 && patch == 0, "API 1.6.0 negotiated");
    expect(counters->setPathTemplate == 1u && std::strcmp(counters->lastPathTemplate, "captures/fuse_wp06") == 0,
           "capture path template forwarded");

    for (fuse::u64 frame = 0; frame < 8; ++frame) {
        capture->onFrameBegin(frame);
        expect(capture->isFrameCapturing() == (frame == 3u || frame == 4u), "capturing exactly frames 3 and 4");
        capture->onFrameEnd(frame);
    }
    expect(counters->startCapture == 1u && counters->endCapture == 1u, "one start/end pair for the scheduled range");
    expect(capture->hookCaptures() == 1u && capture->numCaptures() == 1u, "one capture written");
    expect(counters->lastDevice == dispatch, "device pointer = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE");
    expect(counters->setTitle == 1u && std::strcmp(counters->lastTitle, "FUSE frame 3+1") == 0, "capture titled");

    expect(capture->triggerCapture(), "triggerCapture");
    expect(capture->triggerCapture(4), "triggerCapture(4)");
    expect(counters->triggerCapture == 1u && counters->triggerMultiFrames == 4u, "trigger calls forwarded");
    expect(capture->startFrameCapture() && capture->discardFrameCapture(), "explicit start + discard");
    expect(counters->discardCapture == 1u && !capture->isFrameCapturing(), "discard forwarded");
    // A second instance on the same (already loaded) library shares RenderDoc's state. The double
    // has no librenderdoc.so soname, so it is named again rather than found as injected.
    auto second = RenderDocCapture::create(desc);
    expect(second->available() && second->numCaptures() == 1u, "second instance shares the loaded API");
}
#endif

} // namespace

int main() {
    testUnavailable();
    testTriggers();
#if defined(FUSE_RP_FAKE_RENDERDOC)
    testFakeRenderDoc();
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: RenderDoc capture hook\n");
    return 0;
}
