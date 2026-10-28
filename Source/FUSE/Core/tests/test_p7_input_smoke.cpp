#include <fuse/core/smoke_frame.hpp>
#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input_replay.hpp>
#include <fuse/platform/key_code.hpp>
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

void testSmokeFrameOk() {
    expectTrue(fuse::core::smoke_frame_ok(1), "smoke_frame_ok(1)");
    expectTrue(fuse::core::smoke_frame_ok(120), "smoke_frame_ok(120)");
    expectTrue(!fuse::core::smoke_frame_ok(0), "smoke_frame_ok(0)");
    expectTrue(!fuse::core::smoke_frame_ok(121), "smoke_frame_ok(121)");
}

void testKeyCodeOk() {
    expectTrue(fuse::platform::key_code_ok(1), "key_code_ok(1)");
    expectTrue(fuse::platform::key_code_ok(511), "key_code_ok(511)");
    expectTrue(!fuse::platform::key_code_ok(0), "key_code_ok(0)");
    expectTrue(!fuse::platform::key_code_ok(512), "key_code_ok(512)");
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

void testInputReplayRecordOrderSeekAndReject() {
    fuse::platform::InputReplay replay;

    const fuse::platform::InputEvent first{10u, fuse::platform::Key::A, true};
    const fuse::platform::InputEvent sameTime{10u, fuse::platform::Key::B, false};
    const fuse::platform::InputEvent later{25u, fuse::platform::Key::W, true};
    const fuse::platform::InputEvent earlier{9u, fuse::platform::Key::Escape, true};

    expectTrue(replay.record(first), "first event recorded");
    expectTrue(replay.record(sameTime), "equal timestamp recorded");
    expectTrue(replay.record(later), "later timestamp recorded");
    expectTrue(!replay.record(earlier), "decreasing timestamp rejected");
    expectEqU32(replay.size(), 3u, "rejected event is not stored");

    const auto atStart = replay.next();
    expectTrue(atStart.has_value(), "cursor starts at first recorded event");
    if (atStart.has_value()) {
        expectEqU32(atStart->timeMs, 10u, "record order time 0");
        expectTrue(atStart->key == fuse::platform::Key::A, "record order key A");
        expectTrue(atStart->down, "record order key A down");
    }

    const auto second = replay.next();
    expectTrue(second.has_value(), "second recorded event");
    if (second.has_value()) {
        expectEqU32(second->timeMs, 10u, "record order time 1");
        expectTrue(second->key == fuse::platform::Key::B, "record order key B");
        expectTrue(!second->down, "record order key B up");
    }

    const auto third = replay.next();
    expectTrue(third.has_value(), "third recorded event");
    if (third.has_value()) {
        expectEqU32(third->timeMs, 25u, "record order time 2");
        expectTrue(third->key == fuse::platform::Key::W, "record order key W");
        expectTrue(third->down, "record order key W down");
    }
    expectTrue(!replay.next().has_value(), "next at end is empty");

    replay.seek(11u);
    const auto skipped = replay.next();
    expectTrue(skipped.has_value(), "seek lands on a later event");
    if (skipped.has_value()) {
        expectEqU32(skipped->timeMs, 25u, "seek skips earlier events");
        expectTrue(skipped->key == fuse::platform::Key::W, "seek skips to W");
    }
    expectTrue(!replay.next().has_value(), "no event after seek target");

    replay.seek(10u);
    const auto equalTime = replay.next();
    expectTrue(equalTime.has_value(), "seek to an equal timestamp lands");
    if (equalTime.has_value()) {
        expectTrue(equalTime->key == fuse::platform::Key::A, "seek keeps the first event at that time");
        expectEqU32(equalTime->timeMs, 10u, "seek equal time is not skipped");
    }

    replay.reset();
    const auto resetEvent = replay.next();
    expectTrue(resetEvent.has_value(), "reset returns to the start");
    if (resetEvent.has_value()) {
        expectTrue(resetEvent->key == fuse::platform::Key::A, "reset seeks to time 0");
        expectEqU32(resetEvent->timeMs, 10u, "reset lands on the first recorded event");
    }
}

} // namespace

int main() {
    testSmokeFrameOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core p7 input smoke: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testKeyCodeOk();
    testKeyDownEnqueuesAndPolls();
    testKeyUpEnqueuesAndPolls();
    testMouseMoveEnqueuesAndPolls();
    testMouseButtonDownLeftAtPosition();
    testMouseButtonUp();
    testInputDoesNotBreakResizeCoalesce();
    testDrainEventsFifoIncludesInput();
    testInputReplayRecordOrderSeekAndReject();

    if (g_failures == 0) {
        std::printf("fuse_core p7 input smoke: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core p7 input smoke: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
