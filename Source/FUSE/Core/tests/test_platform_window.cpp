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
    expectTrue(window.isFocused(), "default window starts focused");

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

void testWindowResizeNotifiesPump() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.resize(800, 600, &pump);
    expectEq(pump.pendingEventCount(), 1u, "resize enqueues one event");

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "resize event polled");
    expectTrue(event.type == fuse::platform::PlatformEventType::WindowResized, "resize type");
    expectEq(event.width, 800u, "resize width in event");
    expectEq(event.height, 600u, "resize height in event");
    expectTrue(event.window == &window, "resize window pointer");

    window.resize(800, 600, &pump);
    expectEq(pump.pendingEventCount(), 0u, "no-op resize does not enqueue");
}

void testWindowFocusEventsNotifyPump() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.setFocused(false, &pump);
    expectTrue(!window.isFocused(), "focus cleared");

    fuse::platform::PlatformEvent lost;
    expectTrue(pump.pollEvent(lost), "focus lost polled");
    expectTrue(lost.type == fuse::platform::PlatformEventType::WindowFocusLost, "focus lost type");
    expectTrue(lost.window == &window, "focus lost window pointer");

    window.setFocused(false, &pump);
    expectEq(pump.pendingEventCount(), 0u, "duplicate focus lost does not enqueue");

    window.setFocused(true, &pump);
    fuse::platform::PlatformEvent gained;
    expectTrue(pump.pollEvent(gained), "focus gained polled");
    expectTrue(gained.type == fuse::platform::PlatformEventType::WindowFocusGained,
               "focus gained type");
}

void testWindowCloseRequestedNotifiesPump() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.requestClose(&pump);
    expectTrue(window.closeRequest() == fuse::platform::WindowCloseRequest::Requested,
               "close request recorded with pump");

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "close requested polled");
    expectTrue(event.type == fuse::platform::PlatformEventType::WindowCloseRequested,
               "close requested type");
    expectTrue(event.window == &window, "close requested window pointer");
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
    expectTrue(!pump.hasPendingEvents(), "queue empty after OS drain");
    expectEq(pump.pendingEventCount(), 0u, "pending count zero after OS drain");
}

void testEventPumpPollQueueFifoOrder() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    for (fuse::u32 index = 0; index < 5u; ++index) {
        fuse::platform::PlatformEvent event;
        event.type = fuse::platform::PlatformEventType::WindowResized;
        event.window = &window;
        event.width = 640u + index;
        event.height = 480u + index;
        pump.pushSyntheticEvent(event);
    }

    expectEq(pump.pendingEventCount(), 5u, "five events queued");

    for (fuse::u32 index = 0; index < 5u; ++index) {
        fuse::platform::PlatformEvent polled;
        expectTrue(pump.pollEvent(polled), "fifo poll succeeds");
        expectEq(polled.width, 640u + index, "fifo width order preserved");
        expectEq(polled.height, 480u + index, "fifo height order preserved");
    }

    expectTrue(!pump.hasPendingEvents(), "queue drained");
}

void testEventPumpPollQueueOverflowDropsTail() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    fuse::platform::PlatformEvent marker;
    marker.type = fuse::platform::PlatformEventType::WindowResized;
    marker.window = &window;
    marker.width = 111u;
    marker.height = 222u;
    pump.pushSyntheticEvent(marker);

    for (fuse::u32 index = 0; index < 32u; ++index) {
        fuse::platform::PlatformEvent event;
        event.type = fuse::platform::PlatformEventType::WindowResized;
        event.window = &window;
        event.width = 1000u + index;
        event.height = 2000u + index;
        pump.pushSyntheticEvent(event);
    }

    expectEq(pump.pendingEventCount(), 31u, "ring buffer caps at capacity minus one slot");

    fuse::platform::PlatformEvent first;
    expectTrue(pump.pollEvent(first), "oldest event still available");
    expectEq(first.width, 111u, "marker event preserved at head");
}

void testEventPumpClearSyntheticEvents() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);
    pump.pushWindowFocusLost(window);
    expectTrue(pump.hasPendingEvents(), "events queued before clear");

    pump.clearSyntheticEvents();
    expectEq(pump.pendingEventCount(), 0u, "clear empties queue");

    fuse::platform::PlatformEvent event;
    expectTrue(!pump.pollEvent(event), "poll returns false after clear");
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
    testWindowResizeNotifiesPump();
    testWindowFocusEventsNotifyPump();
    testWindowCloseRequestedNotifiesPump();
    testVulkanSurfaceWireIsHeadless();
    testEventPumpSyntheticEvents();
    testEventPumpQuitFlow();
    testEventPumpOsDrainIsNoOp();
    testEventPumpPollQueueFifoOrder();
    testEventPumpPollQueueOverflowDropsTail();
    testEventPumpClearSyntheticEvents();
    testMobileProfileStillUsesWindowStub();

    if (g_failures == 0) {
        std::printf("fuse_core platform window tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core platform window tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
