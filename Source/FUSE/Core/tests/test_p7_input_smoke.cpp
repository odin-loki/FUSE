#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/window.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEqU32(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

void expectEqI32(fuse::i32 actual, fuse::i32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %d, got %d)\n", message, expected, actual);
        ++g_failures;
    }
}

void testKeyDownEnqueuesAndPolls() {
    fuse::platform::EventPump pump;

    pump.pushKeyDown(65u);

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "KeyDown polled");
    expectTrue(event.type == fuse::platform::PlatformEventType::KeyDown, "KeyDown type");
    expectEqU32(event.keyCode, 65u, "KeyDown keyCode 65");
    expectTrue(event.window == nullptr, "KeyDown has no window");
    expectEqU32(event.mouseButton, 0u, "KeyDown mouseButton unused");
    expectTrue(!pump.hasPendingEvents(), "KeyDown consumed");
}

void testKeyUpEnqueuesAndPolls() {
    fuse::platform::EventPump pump;

    pump.pushKeyUp(65u);

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "KeyUp polled");
    expectTrue(event.type == fuse::platform::PlatformEventType::KeyUp, "KeyUp type");
    expectEqU32(event.keyCode, 65u, "KeyUp keyCode 65");
    expectTrue(!pump.pollEvent(event), "KeyUp consumed");
}

void testMouseMoveEnqueuesAndPolls() {
    fuse::platform::EventPump pump;

    pump.pushMouseMove(120, 40);

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "MouseMove polled");
    expectTrue(event.type == fuse::platform::PlatformEventType::MouseMove, "MouseMove type");
    expectEqI32(event.mouseX, 120, "MouseMove x");
    expectEqI32(event.mouseY, 40, "MouseMove y");
    expectEqU32(event.keyCode, 0u, "MouseMove keyCode unused");
    expectTrue(!pump.hasPendingEvents(), "MouseMove consumed");
}

void testMouseButtonDownLeftAtPosition() {
    fuse::platform::EventPump pump;

    pump.pushMouseMove(120, 40);
    pump.pushMouseButton(1u, true, 120, 40);

    fuse::platform::PlatformEvent move;
    expectTrue(pump.pollEvent(move), "MouseMove before button");
    expectTrue(move.type == fuse::platform::PlatformEventType::MouseMove, "move type before button");

    fuse::platform::PlatformEvent button;
    expectTrue(pump.pollEvent(button), "MouseButtonDown polled");
    expectTrue(button.type == fuse::platform::PlatformEventType::MouseButtonDown,
               "MouseButtonDown type");
    expectEqU32(button.mouseButton, 1u, "left mouse button");
    expectEqI32(button.mouseX, 120, "button down x");
    expectEqI32(button.mouseY, 40, "button down y");
    expectTrue(!pump.hasPendingEvents(), "button down consumed");
}

void testMouseButtonUp() {
    fuse::platform::EventPump pump;

    pump.pushMouseButton(1u, false, 120, 40);

    fuse::platform::PlatformEvent event;
    expectTrue(pump.pollEvent(event), "MouseButtonUp polled");
    expectTrue(event.type == fuse::platform::PlatformEventType::MouseButtonUp, "MouseButtonUp type");
    expectEqU32(event.mouseButton, 1u, "left button up");
    expectEqI32(event.mouseX, 120, "button up x");
    expectEqI32(event.mouseY, 40, "button up y");
}

void testInputDoesNotBreakResizeCoalesce() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    window.resize(800, 600, &pump);
    pump.pushKeyDown(65u);
    pump.pushMouseMove(120, 40);
    window.resize(1024, 768, &pump);

    expectEqU32(pump.pendingEventCount(), 3u, "resize coalesces across input events");
    expectEqU32(pump.coalescedResizeCount(), 1u, "one in-place resize coalesce");

    fuse::platform::PlatformEvent resized;
    expectTrue(pump.pollEvent(resized), "coalesced resize polled first");
    expectTrue(resized.type == fuse::platform::PlatformEventType::WindowResized,
               "front event remains coalesced resize");
    expectEqU32(resized.width, 1024u, "coalesced resize keeps latest width");
    expectEqU32(resized.height, 768u, "coalesced resize keeps latest height");
    expectEqU32(resized.keyCode, 0u, "resize keyCode unused");

    fuse::platform::PlatformEvent key;
    expectTrue(pump.pollEvent(key), "KeyDown after coalesced resize");
    expectTrue(key.type == fuse::platform::PlatformEventType::KeyDown, "input KeyDown preserved");
    expectEqU32(key.keyCode, 65u, "input keyCode preserved across coalesce");

    fuse::platform::PlatformEvent move;
    expectTrue(pump.pollEvent(move), "MouseMove after KeyDown");
    expectTrue(move.type == fuse::platform::PlatformEventType::MouseMove, "input MouseMove preserved");
    expectEqI32(move.mouseX, 120, "input mouseX preserved across coalesce");
    expectEqI32(move.mouseY, 40, "input mouseY preserved across coalesce");
}

void testDrainEventsFifoIncludesInput() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;

    pump.pushKeyDown(65u);
    pump.pushKeyUp(65u);
    pump.pushMouseMove(120, 40);
    pump.pushMouseButton(1u, true, 120, 40);
    pump.pushWindowResized(window);

    std::vector<fuse::platform::PlatformEvent> drained;
    expectEqU32(pump.drainEvents(drained), 5u, "drainEvents moves all input + resize");
    expectEqU32(drained.size(), 5u, "drainEvents appends five events");
    expectTrue(drained[0].type == fuse::platform::PlatformEventType::KeyDown, "fifo KeyDown first");
    expectEqU32(drained[0].keyCode, 65u, "fifo KeyDown keyCode");
    expectTrue(drained[1].type == fuse::platform::PlatformEventType::KeyUp, "fifo KeyUp second");
    expectEqU32(drained[1].keyCode, 65u, "fifo KeyUp keyCode");
    expectTrue(drained[2].type == fuse::platform::PlatformEventType::MouseMove, "fifo MouseMove third");
    expectEqI32(drained[2].mouseX, 120, "fifo MouseMove x");
    expectEqI32(drained[2].mouseY, 40, "fifo MouseMove y");
    expectTrue(drained[3].type == fuse::platform::PlatformEventType::MouseButtonDown,
               "fifo MouseButtonDown fourth");
    expectEqU32(drained[3].mouseButton, 1u, "fifo left button");
    expectEqI32(drained[3].mouseX, 120, "fifo button x");
    expectEqI32(drained[3].mouseY, 40, "fifo button y");
    expectTrue(drained[4].type == fuse::platform::PlatformEventType::WindowResized,
               "fifo WindowResized last");
    expectEqU32(pump.pendingEventCount(), 0u, "drain empties queue including input");
}

} // namespace

int main() {
    testKeyDownEnqueuesAndPolls();
    testKeyUpEnqueuesAndPolls();
    testMouseMoveEnqueuesAndPolls();
    testMouseButtonDownLeftAtPosition();
    testMouseButtonUp();
    testInputDoesNotBreakResizeCoalesce();
    testDrainEventsFifoIncludesInput();

    if (g_failures == 0) {
        std::printf("fuse_core p7 input smoke: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core p7 input smoke: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
