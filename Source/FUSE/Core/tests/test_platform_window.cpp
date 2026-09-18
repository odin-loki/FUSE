#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/profile.hpp>
#include <fuse/platform/window.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

    window.requestClose(&pump);
    expectEq(pump.pendingEventCount(), 0u, "duplicate close request does not enqueue");
}

void testWindowFocusEdgeTransitions() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.setFocused(false, &pump);
    window.setFocused(false, &pump);
    expectEq(pump.pendingEventCount(), 1u, "only one focus-lost when already blurred");

    window.setFocused(true, &pump);
    window.setFocused(true, &pump);
    expectEq(pump.pendingEventCount(), 2u, "only one focus-gained when already focused");

    fuse::platform::PlatformEvent first;
    expectTrue(pump.pollEvent(first), "focus lost polled first");
    expectTrue(first.type == fuse::platform::PlatformEventType::WindowFocusLost,
               "first edge event is focus lost");

    fuse::platform::PlatformEvent second;
    expectTrue(pump.pollEvent(second), "focus gained polled second");
    expectTrue(second.type == fuse::platform::PlatformEventType::WindowFocusGained,
               "second edge event is focus gained");
    expectTrue(!pump.hasPendingEvents(), "focus edge sequence fully drained");
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

    pump.pushWindowResized(window);
    pump.pushWindowFocusLost(window);
    pump.pushWindowFocusGained(window);
    pump.pushWindowCloseRequested(window);
    pump.requestQuit();

    expectEq(pump.pendingEventCount(), 5u, "five distinct events queued");

    const fuse::platform::PlatformEventType expectedOrder[] = {
        fuse::platform::PlatformEventType::WindowResized,
        fuse::platform::PlatformEventType::WindowFocusLost,
        fuse::platform::PlatformEventType::WindowFocusGained,
        fuse::platform::PlatformEventType::WindowCloseRequested,
        fuse::platform::PlatformEventType::Quit,
    };

    for (fuse::u32 index = 0; index < 5u; ++index) {
        fuse::platform::PlatformEvent polled;
        expectTrue(pump.pollEvent(polled), "fifo poll succeeds");
        expectTrue(polled.type == expectedOrder[index], "fifo event type order preserved");
    }

    expectTrue(!pump.hasPendingEvents(), "queue drained");
}

void testEventPumpPollQueueOverflowDropsTail() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);

    fuse::platform::PlatformEvent marker;
    marker.type = fuse::platform::PlatformEventType::WindowFocusLost;
    marker.window = &window;
    pump.pushSyntheticEvent(marker);

    for (fuse::u32 index = 0; index < 32u; ++index) {
        fuse::platform::PlatformEvent event;
        event.type = fuse::platform::PlatformEventType::WindowFocusGained;
        event.window = &window;
        pump.pushSyntheticEvent(event);
    }

    expectEq(pump.pendingEventCount(), 31u, "ring buffer caps at capacity minus one slot");

    fuse::platform::PlatformEvent first;
    expectTrue(pump.pollEvent(first), "oldest event still available");
    expectTrue(first.type == fuse::platform::PlatformEventType::WindowResized,
               "marker resize event preserved at head");
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

void testEventPumpCoalesceResizeEvents() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.resize(800, 600, &pump);
    window.resize(1024, 768, &pump);
    expectEq(pump.pendingEventCount(), 1u, "duplicate resize coalesces to one event");

    fuse::platform::PlatformEvent coalesced;
    expectTrue(pump.pollEvent(coalesced), "coalesced resize polled");
    expectEq(coalesced.width, 1024u, "coalesced resize keeps latest width");
    expectEq(coalesced.height, 768u, "coalesced resize keeps latest height");

    pump.pushWindowResized(window);
    pump.pushWindowFocusLost(window);
    window.resize(640, 480, &pump);
    expectEq(pump.pendingEventCount(), 2u, "resize coalesces without reordering focus event");

    fuse::platform::PlatformEvent resized;
    expectTrue(pump.pollEvent(resized), "coalesced resize before focus event");
    expectEq(resized.width, 640u, "coalesced resize before focus keeps latest width");
    expectEq(resized.height, 480u, "coalesced resize before focus keeps latest height");

    fuse::platform::PlatformEvent focusLost;
    expectTrue(pump.pollEvent(focusLost), "focus event preserved after resize coalesce");
    expectTrue(focusLost.type == fuse::platform::PlatformEventType::WindowFocusLost,
               "focus event type preserved after resize coalesce");
}

void testEventPumpDrainEventsEmptyQueue() {
    fuse::platform::EventPump pump;

    expectTrue(!pump.hasPendingEvents(), "fresh pump has empty queue");
    expectEq(pump.pendingEventCount(), 0u, "pending count zero on fresh pump");

    std::vector<fuse::platform::PlatformEvent> drained;
    expectEq(pump.drainEvents(drained), 0u, "drainEvents on empty queue returns zero");
    expectTrue(drained.empty(), "drainEvents leaves output vector empty");
    expectTrue(!pump.quitRequested(), "empty drain does not request quit");
}

void testEventPumpDrainEventsHelper() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);
    pump.pushWindowFocusGained(window);
    pump.requestQuit();

    std::vector<fuse::platform::PlatformEvent> drained;
    expectEq(pump.drainEvents(drained), 3u, "drainEvents returns moved count");
    expectEq(drained.size(), 3u, "drainEvents appends all queued events");
    expectTrue(drained[0].type == fuse::platform::PlatformEventType::WindowResized,
               "drain preserves fifo resize first");
    expectTrue(drained[1].type == fuse::platform::PlatformEventType::WindowFocusGained,
               "drain preserves fifo focus second");
    expectTrue(drained[2].type == fuse::platform::PlatformEventType::Quit,
               "drain preserves fifo quit last");
    expectEq(pump.pendingEventCount(), 0u, "drain empties queue");
    expectTrue(pump.quitRequested(), "drain still marks quit when Quit event polled");

    expectEq(pump.drainEvents(drained), 0u, "second drain on empty queue returns zero");
    expectEq(drained.size(), 3u, "second drain appends nothing");
}

void testEventPumpCoalesceIsPerWindow() {
    fuse::platform::EventPump pump;
    fuse::platform::Window left;
    fuse::platform::Window right;

    left.resize(800, 600, &pump);
    right.resize(1024, 768, &pump);
    left.resize(1280, 720, &pump);

    expectEq(pump.pendingEventCount(), 2u, "coalesce does not merge resizes across windows");

    fuse::platform::PlatformEvent first;
    expectTrue(pump.pollEvent(first), "left resize polled first");
    expectTrue(first.window == &left, "first resize belongs to left window");
    expectEq(first.width, 1280u, "left resize coalesced to latest width");

    fuse::platform::PlatformEvent second;
    expectTrue(pump.pollEvent(second), "right resize polled second");
    expectTrue(second.window == &right, "second resize belongs to right window");
    expectEq(second.width, 1024u, "right resize keeps its own dimensions");
}

void testEventPumpPeekEventEmptyQueueGuard() {
    fuse::platform::EventPump pump;

    fuse::platform::PlatformEvent peeked;
    peeked.type = fuse::platform::PlatformEventType::WindowCloseRequested;
    expectTrue(!pump.peekEvent(peeked), "peekEvent returns false on empty queue");
    expectTrue(peeked.type == fuse::platform::PlatformEventType::None,
               "peekEvent resets outEvent on empty queue");
}

void testEventPumpPeekEventDoesNotPop() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);
    expectEq(pump.pendingEventCount(), 1u, "resize queued before peek");

    fuse::platform::PlatformEvent peeked;
    expectTrue(pump.peekEvent(peeked), "peekEvent succeeds with pending event");
    expectTrue(peeked.type == fuse::platform::PlatformEventType::WindowResized, "peek sees resize");
    expectEq(pump.pendingEventCount(), 1u, "peek does not remove queued event");

    fuse::platform::PlatformEvent polled;
    expectTrue(pump.pollEvent(polled), "poll still drains after peek");
    expectTrue(polled.type == fuse::platform::PlatformEventType::WindowResized, "polled resize matches peek");
}

void testEventPumpPollEventEmptyQueueGuard() {
    fuse::platform::EventPump pump;

    fuse::platform::PlatformEvent event;
    event.type = fuse::platform::PlatformEventType::Quit;
    expectTrue(!pump.pollEvent(event), "pollEvent returns false on empty queue");
    expectTrue(event.type == fuse::platform::PlatformEventType::None,
               "pollEvent resets outEvent on empty queue");
}

void testEventPumpDroppedEventCount() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    expectEq(pump.droppedEventCount(), 0u, "no drops on fresh pump");

    pump.pushWindowResized(window);
    fuse::platform::PlatformEvent marker;
    marker.type = fuse::platform::PlatformEventType::WindowFocusLost;
    marker.window = &window;
    pump.pushSyntheticEvent(marker);

    for (fuse::u32 index = 0; index < 32u; ++index) {
        fuse::platform::PlatformEvent event;
        event.type = fuse::platform::PlatformEventType::WindowFocusGained;
        event.window = &window;
        pump.pushSyntheticEvent(event);
    }

    expectEq(pump.droppedEventCount(), 3u, "overflow increments droppedEventCount for each tail drop");
    expectEq(pump.pendingEventCount(), 31u, "ring buffer still capped after overflow");
}

void testEventPumpCoalescedResizeCount() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    expectEq(pump.coalescedResizeCount(), 0u, "no coalesces on fresh pump");

    window.resize(800, 600, &pump);
    window.resize(1024, 768, &pump);
    window.resize(1280, 720, &pump);
    expectEq(pump.coalescedResizeCount(), 2u, "two duplicate resizes coalesced");
    expectEq(pump.pendingEventCount(), 1u, "coalesced resizes leave one queued event");

    pump.resetEventStats();
    expectEq(pump.coalescedResizeCount(), 0u, "resetEventStats clears coalesce counter");
    expectEq(pump.pendingEventCount(), 1u, "resetEventStats does not drain queue");
}

void testEventPumpPeekEventTypeEmptyQueueGuard() {
    fuse::platform::EventPump pump;

    fuse::platform::PlatformEventType type = fuse::platform::PlatformEventType::Quit;
    expectTrue(!pump.peekEventType(type), "peekEventType returns false on empty queue");
    expectTrue(type == fuse::platform::PlatformEventType::None,
               "peekEventType resets outType on empty queue");
}

void testEventPumpPeekEventTypeDoesNotPop() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowCloseRequested(window);
    expectEq(pump.pendingEventCount(), 1u, "close requested queued before type peek");

    fuse::platform::PlatformEventType type = fuse::platform::PlatformEventType::None;
    expectTrue(pump.peekEventType(type), "peekEventType succeeds with pending event");
    expectTrue(type == fuse::platform::PlatformEventType::WindowCloseRequested,
               "peekEventType sees close requested");
    expectEq(pump.pendingEventCount(), 1u, "peekEventType does not remove queued event");
}

void testEventPumpPendingResizeExtentFor() {
    fuse::platform::EventPump pump;
    fuse::platform::Window left;
    fuse::platform::Window right;

    const fuse::platform::PendingResizeExtent emptyLeft = pump.pendingResizeExtentFor(left);
    expectTrue(!emptyLeft.pending, "empty queue reports no pending resize extent");
    expectEq(emptyLeft.width, 0u, "empty resize extent width is zero");
    expectEq(emptyLeft.height, 0u, "empty resize extent height is zero");

    left.resize(800, 600, &pump);
    const fuse::platform::PendingResizeExtent first = pump.pendingResizeExtentFor(left);
    expectTrue(first.pending, "resize extent pending after enqueue");
    expectEq(first.width, 800u, "resize extent width matches enqueue");
    expectEq(first.height, 600u, "resize extent height matches enqueue");

    left.resize(1024, 768, &pump);
    const fuse::platform::PendingResizeExtent coalesced = pump.pendingResizeExtentFor(left);
    expectTrue(coalesced.pending, "coalesced resize extent still pending");
    expectEq(coalesced.width, 1024u, "coalesced resize extent keeps latest width");
    expectEq(coalesced.height, 768u, "coalesced resize extent keeps latest height");
    expectTrue(!pump.pendingResizeExtentFor(right).pending,
               "other window has no pending resize extent");

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "drain coalesced resize");
    expectTrue(!pump.pendingResizeExtentFor(left).pending,
               "resize extent cleared after poll");
}

void testEventPumpLastCoalescedResizeRecord() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    expectTrue(!pump.lastCoalescedResize().valid, "no coalesce record on fresh pump");

    window.resize(800, 600, &pump);
    window.resize(1280, 720, &pump);
    const fuse::platform::ResizeCoalesceRecord& record = pump.lastCoalescedResize();
    expectTrue(record.valid, "coalesce record valid after merge");
    expectTrue(record.window == &window, "coalesce record stores window pointer");
    expectEq(record.width, 1280u, "coalesce record keeps latest width");
    expectEq(record.height, 720u, "coalesce record keeps latest height");

    pump.resetEventStats();
    expectTrue(!pump.lastCoalescedResize().valid, "resetEventStats clears coalesce record");
    expectEq(pump.pendingEventCount(), 1u, "resetEventStats does not drain coalesced resize");
}

void testEventPumpStatsSnapshot() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    const fuse::platform::EventPumpStats fresh = pump.stats();
    expectEq(fresh.pendingEventCount, 0u, "fresh stats pending count is zero");
    expectEq(fresh.droppedEventCount, 0u, "fresh stats dropped count is zero");
    expectEq(fresh.coalescedResizeCount, 0u, "fresh stats coalesce count is zero");
    expectTrue(!fresh.quitRequested, "fresh stats quit flag is false");
    expectTrue(!fresh.hasPendingQuitEvent, "fresh stats quit-event flag is false");

    window.resize(800, 600, &pump);
    window.resize(1024, 768, &pump);
    pump.requestQuit();

    const fuse::platform::EventPumpStats active = pump.stats();
    expectEq(active.pendingEventCount, 2u, "stats pending count tracks queued events");
    expectEq(active.coalescedResizeCount, 1u, "stats coalesce count tracks merges");
    expectTrue(active.quitRequested, "stats quit flag tracks requestQuit");
    expectTrue(active.hasPendingQuitEvent, "stats quit-event flag tracks queued Quit");
}

void testEventPumpPumpOnceEmptyQueueEarlyOut() {
    fuse::platform::EventPump pump;

    expectTrue(!pump.hasPendingEvents(), "fresh pump queue is empty");
    expectTrue(pump.pumpOnce(), "pumpOnce early-outs on empty queue");
    expectTrue(!pump.quitRequested(), "empty pumpOnce does not request quit");
    expectEq(pump.pendingEventCount(), 0u, "empty pumpOnce leaves queue empty");
}

void testEventPumpHasPendingResizeFor() {
    fuse::platform::EventPump pump;
    fuse::platform::Window left;
    fuse::platform::Window right;

    expectTrue(!pump.hasPendingResizeFor(left), "empty queue has no pending resize");
    expectTrue(!pump.hasPendingResizeFor(right), "empty queue has no pending resize for right");

    left.resize(800, 600, &pump);
    expectTrue(pump.hasPendingResizeFor(left), "left resize is pending");
    expectTrue(!pump.hasPendingResizeFor(right), "right window has no pending resize yet");

    pump.pushWindowFocusLost(left);
    left.resize(1024, 768, &pump);
    expectTrue(pump.hasPendingResizeFor(left), "coalesced resize still pending for left");
    expectEq(pump.pendingEventCount(), 2u, "resize + focus events remain distinct");

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "drain resize event");
    expectTrue(!pump.hasPendingResizeFor(left), "resize no longer pending after poll");
    expectTrue(pump.hasPendingEvents(), "focus event still pending");
}

void testEventPumpHasPendingEventOfType() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    expectTrue(!pump.hasPendingEventOfType(fuse::platform::PlatformEventType::Quit),
               "empty queue has no pending quit event");
    expectTrue(!pump.hasPendingEventOfType(fuse::platform::PlatformEventType::WindowResized),
               "empty queue has no pending resize event");

    pump.pushWindowResized(window);
    expectTrue(pump.hasPendingEventOfType(fuse::platform::PlatformEventType::WindowResized),
               "resize type detected in queue");
    expectTrue(!pump.hasPendingEventOfType(fuse::platform::PlatformEventType::Quit),
               "quit not present before requestQuit");

    pump.requestQuit();
    expectTrue(pump.hasPendingEventOfType(fuse::platform::PlatformEventType::Quit),
               "quit type detected after requestQuit");
    expectTrue(pump.hasPendingEventOfType(fuse::platform::PlatformEventType::WindowResized),
               "resize type still present alongside quit");
}

void testEventPumpCountPendingEventsFor() {
    fuse::platform::EventPump pump;
    fuse::platform::Window left;
    fuse::platform::Window right;

    expectEq(pump.countPendingEventsFor(left), 0u, "empty queue has zero events for left");
    expectEq(pump.countPendingEventsFor(right), 0u, "empty queue has zero events for right");

    left.resize(800, 600, &pump);
    pump.pushWindowFocusLost(left);
    right.resize(1024, 768, &pump);
    expectEq(pump.countPendingEventsFor(left), 2u, "left has resize and focus events");
    expectEq(pump.countPendingEventsFor(right), 1u, "right has one resize event");

    left.resize(1280, 720, &pump);
    expectEq(pump.countPendingEventsFor(left), 2u, "coalesced resize does not add left count");
    expectEq(pump.pendingEventCount(), 3u, "three distinct queued events remain");
}

void testEventPumpPendingResizeExtentZeroDimensionGuard() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    fuse::platform::PlatformEvent invalid;
    invalid.type = fuse::platform::PlatformEventType::WindowResized;
    invalid.window = &window;
    invalid.width = 0;
    invalid.height = 768;
    pump.pushSyntheticEvent(invalid);

    const fuse::platform::PendingResizeExtent partialWidth = pump.pendingResizeExtentFor(window);
    expectTrue(!partialWidth.pending, "zero width resize extent is not pending");
    expectTrue(!pump.hasPendingResizeFor(window), "zero width resize does not count as pending");

    fuse::platform::PlatformEvent invalidHeight;
    invalidHeight.type = fuse::platform::PlatformEventType::WindowResized;
    invalidHeight.window = &window;
    invalidHeight.width = 640;
    invalidHeight.height = 0;
    pump.pushSyntheticEvent(invalidHeight);

    const fuse::platform::PendingResizeExtent partialHeight = pump.pendingResizeExtentFor(window);
    expectTrue(!partialHeight.pending, "zero height resize extent is not pending");

    window.resize(800, 600, &pump);
    const fuse::platform::PendingResizeExtent valid = pump.pendingResizeExtentFor(window);
    expectTrue(valid.pending, "valid resize extent remains pending after invalid entries");
    expectEq(valid.width, 800u, "valid resize extent keeps latest width");
    expectEq(valid.height, 600u, "valid resize extent keeps latest height");
}

void testEventPumpStatsDroppedCountAndQuitEvent() {
    fuse::platform::EventPump overflowPump;
    fuse::platform::Window window;

    overflowPump.pushWindowResized(window);
    fuse::platform::PlatformEvent marker;
    marker.type = fuse::platform::PlatformEventType::WindowFocusLost;
    marker.window = &window;
    overflowPump.pushSyntheticEvent(marker);

    for (fuse::u32 index = 0; index < 32u; ++index) {
        fuse::platform::PlatformEvent event;
        event.type = fuse::platform::PlatformEventType::WindowFocusGained;
        event.window = &window;
        overflowPump.pushSyntheticEvent(event);
    }

    const fuse::platform::EventPumpStats overflow = overflowPump.stats();
    expectEq(overflow.droppedEventCount, 3u, "stats dropped count tracks overflow");
    expectEq(overflow.pendingEventCount, 31u, "stats pending count tracks capped queue");
    expectTrue(!overflow.hasPendingQuitEvent, "stats quit-event flag false before requestQuit");

    fuse::platform::EventPump quitPump;
    quitPump.requestQuit();
    const fuse::platform::EventPumpStats withQuit = quitPump.stats();
    expectTrue(withQuit.hasPendingQuitEvent, "stats quit-event flag true when Quit queued");
    expectTrue(withQuit.quitRequested, "stats quit flag true after requestQuit");
}

void testEventPumpPumpOnceQuitFlagWithoutQueuedEvents() {
    fuse::platform::EventPump pump;

    pump.requestQuit();
    fuse::platform::PlatformEvent quit;
    expectTrue(pump.pollEvent(quit), "quit event polled");
    expectTrue(quit.type == fuse::platform::PlatformEventType::Quit, "quit event type");
    expectTrue(pump.quitRequested(), "quit flag remains set after poll");
    expectTrue(!pump.hasPendingEvents(), "queue empty after quit poll");

    expectTrue(!pump.pumpOnce(), "pumpOnce returns false when quit flagged with empty queue");
    expectEq(pump.pendingEventCount(), 0u, "empty quit pumpOnce leaves queue empty");
}

void testEventPumpTryPeekEventOfTypeEmptyQueueGuard() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    fuse::platform::PlatformEvent peeked;
    peeked.type = fuse::platform::PlatformEventType::Quit;
    expectTrue(!pump.tryPeekEventOfType(fuse::platform::PlatformEventType::WindowResized, peeked),
               "tryPeekEventOfType returns false on empty queue");
    expectTrue(peeked.type == fuse::platform::PlatformEventType::None,
               "tryPeekEventOfType resets outEvent on empty queue");
}

void testEventPumpTryPeekEventOfTypeTypeMismatchGuard() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowFocusLost(window);
    expectEq(pump.pendingEventCount(), 1u, "focus lost queued before type peek");

    fuse::platform::PlatformEvent peeked;
    peeked.type = fuse::platform::PlatformEventType::Quit;
    expectTrue(!pump.tryPeekEventOfType(fuse::platform::PlatformEventType::WindowResized, peeked),
               "tryPeekEventOfType returns false when front type mismatches");
    expectTrue(peeked.type == fuse::platform::PlatformEventType::None,
               "tryPeekEventOfType resets outEvent on type mismatch");
    expectEq(pump.pendingEventCount(), 1u, "type mismatch peek does not remove queued event");
}

void testEventPumpTryPeekEventOfTypeMatchesFront() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.resize(800, 600, &pump);
    fuse::platform::PlatformEvent peeked;
    expectTrue(pump.tryPeekEventOfType(fuse::platform::PlatformEventType::WindowResized, peeked),
               "tryPeekEventOfType succeeds when front matches");
    expectEq(peeked.width, 800u, "tryPeekEventOfType copies resize width");
    expectEq(peeked.height, 600u, "tryPeekEventOfType copies resize height");
    expectTrue(peeked.window == &window, "tryPeekEventOfType copies window pointer");
    expectEq(pump.pendingEventCount(), 1u, "tryPeekEventOfType does not pop");
}

void testEventPumpPeekAfterCoalesceUpdatesFront() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.resize(800, 600, &pump);
    window.resize(1280, 720, &pump);
    expectEq(pump.coalescedResizeCount(), 1u, "one resize coalesced before peek");

    fuse::platform::PlatformEvent peeked;
    expectTrue(pump.peekEvent(peeked), "peek succeeds after coalesce");
    expectEq(peeked.width, 1280u, "peek sees coalesced width");
    expectEq(peeked.height, 720u, "peek sees coalesced height");

    fuse::platform::PlatformEventType type = fuse::platform::PlatformEventType::None;
    expectTrue(pump.peekEventType(type), "peekEventType succeeds after coalesce");
    expectTrue(type == fuse::platform::PlatformEventType::WindowResized,
               "peekEventType sees resize after coalesce");

    fuse::platform::PlatformEvent typed;
    expectTrue(pump.tryPeekEventOfType(fuse::platform::PlatformEventType::WindowResized, typed),
               "tryPeekEventOfType succeeds after coalesce");
    expectEq(typed.width, 1280u, "tryPeekEventOfType sees coalesced width");
}

void testEventPumpCountPendingEventsOfTypeEmptyQueueGuard() {
    fuse::platform::EventPump pump;

    expectEq(pump.countPendingEventsOfType(fuse::platform::PlatformEventType::WindowResized), 0u,
             "countPendingEventsOfType zero on empty queue");
    expectEq(pump.countPendingEventsOfType(fuse::platform::PlatformEventType::Quit), 0u,
             "countPendingEventsOfType zero for quit on empty queue");
    expectTrue(!pump.hasPendingEventOfType(fuse::platform::PlatformEventType::Quit),
               "hasPendingEventOfType false on empty queue");
}

void testEventPumpCountPendingEventsOfType() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);
    pump.pushWindowFocusLost(window);
    pump.pushWindowFocusGained(window);
    pump.requestQuit();

    expectEq(pump.countPendingEventsOfType(fuse::platform::PlatformEventType::WindowResized), 1u,
             "one resize in mixed queue");
    expectEq(pump.countPendingEventsOfType(fuse::platform::PlatformEventType::WindowFocusLost), 1u,
             "one focus lost in mixed queue");
    expectEq(pump.countPendingEventsOfType(fuse::platform::PlatformEventType::Quit), 1u,
             "one quit in mixed queue");
    expectEq(pump.countPendingEventsOfType(fuse::platform::PlatformEventType::WindowCloseRequested),
             0u,
             "zero close requested in mixed queue");

    window.resize(1024, 768, &pump);
    expectEq(pump.countPendingEventsOfType(fuse::platform::PlatformEventType::WindowResized), 1u,
             "coalesced resize does not increase type count");
}

void testEventPumpCoalesceInvalidDimensionGuard() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.resize(800, 600, &pump);
    expectEq(pump.coalescedResizeCount(), 0u, "first resize is not coalesced");

    fuse::platform::PlatformEvent invalid;
    invalid.type = fuse::platform::PlatformEventType::WindowResized;
    invalid.window = &window;
    invalid.width = 0;
    invalid.height = 720;
    pump.pushSyntheticEvent(invalid);
    expectEq(pump.coalescedResizeCount(), 0u, "zero-width resize does not coalesce");
    expectEq(pump.pendingEventCount(), 2u, "invalid resize enqueued separately");

    const fuse::platform::PendingResizeExtent extent = pump.pendingResizeExtentFor(window);
    expectTrue(extent.pending, "valid resize extent still pending");
    expectEq(extent.width, 800u, "valid resize extent not clobbered by invalid coalesce");
    expectEq(extent.height, 600u, "valid resize extent height preserved");

    fuse::platform::PlatformEvent polled;
    expectTrue(pump.pollEvent(polled), "valid resize polled first");
    expectEq(polled.width, 800u, "valid resize drained before invalid entry");
}

void testEventPumpIntrospectionAfterRingWrap() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    for (fuse::u32 index = 0; index < 25u; ++index) {
        fuse::platform::PlatformEvent event;
        event.type = fuse::platform::PlatformEventType::WindowFocusGained;
        event.window = &window;
        pump.pushSyntheticEvent(event);
    }

    for (fuse::u32 index = 0; index < 15u; ++index) {
        fuse::platform::PlatformEvent drained;
        expectTrue(pump.pollEvent(drained), "drain events to advance head");
    }

    for (fuse::u32 index = 0; index < 15u; ++index) {
        fuse::platform::PlatformEvent event;
        event.type = fuse::platform::PlatformEventType::WindowFocusGained;
        event.window = &window;
        pump.pushSyntheticEvent(event);
    }

    pump.pushWindowResized(window);

    expectEq(pump.pendingEventCount(), 26u, "wrapped queue retains pending count");
    expectEq(pump.countPendingEventsOfType(fuse::platform::PlatformEventType::WindowFocusGained), 25u,
             "wrapped queue type count correct");
    expectTrue(pump.hasPendingEventOfType(fuse::platform::PlatformEventType::WindowResized),
               "wrapped queue detects resize type");

    fuse::platform::PlatformEventType frontType = fuse::platform::PlatformEventType::None;
    expectTrue(pump.peekEventType(frontType), "peekEventType succeeds after ring wrap");
    expectTrue(frontType == fuse::platform::PlatformEventType::WindowFocusGained,
               "peekEventType sees fifo head after ring wrap");

    fuse::platform::PlatformEvent typed;
    expectTrue(!pump.tryPeekEventOfType(fuse::platform::PlatformEventType::WindowResized, typed),
               "tryPeekEventOfType false when front type differs after wrap");
}

void testEventPumpClearSyntheticEventsPreservesQuitFlag() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushWindowResized(window);
    pump.requestQuit();
    pump.clearSyntheticEvents();

    expectTrue(pump.quitRequested(), "clearSyntheticEvents preserves quit flag");
    expectEq(pump.pendingEventCount(), 0u, "clearSyntheticEvents empties queue");
    expectTrue(!pump.hasPendingEventOfType(fuse::platform::PlatformEventType::Quit),
               "cleared queue has no pending quit event");

    const fuse::platform::EventPumpStats cleared = pump.stats();
    expectTrue(cleared.quitRequested, "stats quit flag preserved after clear");
    expectTrue(!cleared.hasPendingQuitEvent, "stats quit-event flag false after clear");
    expectEq(cleared.pendingEventCount, 0u, "stats pending count zero after clear");
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
    testWindowFocusEdgeTransitions();
    testVulkanSurfaceWireIsHeadless();
    testEventPumpSyntheticEvents();
    testEventPumpQuitFlow();
    testEventPumpOsDrainIsNoOp();
    testEventPumpPollQueueFifoOrder();
    testEventPumpPollQueueOverflowDropsTail();
    testEventPumpClearSyntheticEvents();
    testEventPumpCoalesceResizeEvents();
    testEventPumpDrainEventsEmptyQueue();
    testEventPumpDrainEventsHelper();
    testEventPumpCoalesceIsPerWindow();
    testEventPumpPeekEventEmptyQueueGuard();
    testEventPumpPeekEventDoesNotPop();
    testEventPumpPeekEventTypeEmptyQueueGuard();
    testEventPumpPeekEventTypeDoesNotPop();
    testEventPumpPollEventEmptyQueueGuard();
    testEventPumpDroppedEventCount();
    testEventPumpCoalescedResizeCount();
    testEventPumpPendingResizeExtentFor();
    testEventPumpLastCoalescedResizeRecord();
    testEventPumpStatsSnapshot();
    testEventPumpPumpOnceEmptyQueueEarlyOut();
    testEventPumpHasPendingResizeFor();
    testEventPumpHasPendingEventOfType();
    testEventPumpCountPendingEventsFor();
    testEventPumpPendingResizeExtentZeroDimensionGuard();
    testEventPumpStatsDroppedCountAndQuitEvent();
    testEventPumpPumpOnceQuitFlagWithoutQueuedEvents();
    testEventPumpTryPeekEventOfTypeEmptyQueueGuard();
    testEventPumpTryPeekEventOfTypeTypeMismatchGuard();
    testEventPumpTryPeekEventOfTypeMatchesFront();
    testEventPumpPeekAfterCoalesceUpdatesFront();
    testEventPumpCountPendingEventsOfTypeEmptyQueueGuard();
    testEventPumpCountPendingEventsOfType();
    testEventPumpCoalesceInvalidDimensionGuard();
    testEventPumpIntrospectionAfterRingWrap();
    testEventPumpClearSyntheticEventsPreservesQuitFlag();
    testMobileProfileStillUsesWindowStub();

    if (g_failures == 0) {
        std::printf("fuse_core platform window tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core platform window tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
