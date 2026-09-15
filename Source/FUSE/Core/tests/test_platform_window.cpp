#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/profile.hpp>
#include <fuse/platform/window.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

void testWindowStubStoresDescription() {
    fuse::platform::WindowDesc desc;
    desc.title = "Smoke";
    desc.width = 1280;
    desc.height = 720;
    desc.fullscreen = true;
    desc.borderless = true;
    desc.vsync = false;

    fuse::platform::Window window(desc);
    expectTrue(window.isValid(), "window stub is valid after construction");

    const fuse::platform::WindowDesc stored = window.description();
    expectTrue(stored.title != nullptr && std::string(stored.title) == "Smoke", "title stored");
    expectEq(window.width(), 1280u, "width stored");
    expectEq(window.height(), 720u, "height stored");
    expectTrue(window.isFullscreen(), "fullscreen flag stored");
    expectTrue(!window.vsyncEnabled(), "vsync flag stored");
    expectTrue(window.nativeHandle().value == nullptr, "native handle is null in stub");
}

void testWindowResizeAndCloseRequest() {
    fuse::platform::Window window;
    expectTrue(window.isValid(), "default window is valid");

    window.resize(800, 600);
    expectEq(window.width(), 800u, "resize updates width");
    expectEq(window.height(), 600u, "resize updates height");

    window.requestClose();
    expectTrue(window.closeRequest() == fuse::platform::WindowCloseRequest::Requested,
               "close request recorded");
    window.clearCloseRequest();
    expectTrue(window.closeRequest() == fuse::platform::WindowCloseRequest::None,
               "close request cleared");
}

void testVulkanSurfaceWireIsHeadless() {
    fuse::platform::Window window;
    const fuse::platform::VulkanSurfaceWire wire = window.vulkanSurfaceWire();

    expectTrue(wire.nativeSurface == nullptr, "stub Vulkan surface is null");
    expectTrue(!wire.presentable, "stub surface is not presentable");
    expectTrue(window.nativeVulkanSurface() == nullptr, "nativeVulkanSurface returns null");
}

void testEventPumpSyntheticEvents() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    fuse::platform::PlatformEvent resizeEvent;
    resizeEvent.type = fuse::platform::PlatformEventType::WindowResized;
    resizeEvent.window = &window;
    resizeEvent.width = 1024;
    resizeEvent.height = 768;
    pump.pushSyntheticEvent(resizeEvent);

    fuse::platform::PlatformEvent polled;
    expectTrue(pump.pollEvent(polled), "synthetic resize event polled");
    expectTrue(polled.type == fuse::platform::PlatformEventType::WindowResized, "resize type preserved");
    expectEq(polled.width, 1024u, "resize width preserved");
    expectEq(polled.height, 768u, "resize height preserved");
    expectTrue(polled.window == &window, "window pointer preserved");
}

void testEventPumpQuitFlow() {
    fuse::platform::EventPump pump;
    expectTrue(pump.pumpOnce(), "pumpOnce succeeds with no events");
    expectTrue(!pump.quitRequested(), "quit not requested initially");

    pump.requestQuit();
    expectTrue(pump.quitRequested(), "quit flag set");
    expectTrue(!pump.pumpOnce(), "pumpOnce returns false after quit");

    pump.resetQuit();
    expectTrue(!pump.quitRequested(), "quit reset");
    expectTrue(pump.pumpOnce(), "pumpOnce succeeds after reset");
}

void testEventPumpOsDrainIsNoOp() {
    fuse::platform::EventPump pump;
    pump.processOsEvents();

    fuse::platform::PlatformEvent event;
    expectTrue(!pump.pollEvent(event), "OS drain produces no events in stub");
}

void testMobileProfileStillUsesWindowStub() {
    const fuse::platform::PlatformProfile previous =
        fuse::platform::setActiveProfileOverride(fuse::platform::PlatformProfile::Mobile);

    fuse::platform::Window window;
    expectTrue(window.isValid(), "mobile profile uses same window stub");
    expectTrue(window.nativeHandle().value == nullptr, "mobile native handle remains null");

    fuse::platform::EventPump pump;
    expectTrue(pump.pumpOnce(), "mobile event pump remains no-op");

    fuse::platform::setActiveProfileOverride(previous);
    fuse::platform::clearActiveProfileOverride();
}

} // namespace

int main() {
    testWindowStubStoresDescription();
    testWindowResizeAndCloseRequest();
    testVulkanSurfaceWireIsHeadless();
    testEventPumpSyntheticEvents();
    testEventPumpQuitFlow();
    testEventPumpOsDrainIsNoOp();
    testMobileProfileStillUsesWindowStub();

    if (g_failures == 0) {
        std::printf("fuse_core platform window tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core platform window tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
