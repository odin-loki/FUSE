#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/platform/window.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEqI32(fuse::i32 actual, fuse::i32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %d, got %d)\n", message, expected, actual);
        ++g_failures;
    }
}

fuse::platform::PlatformEvent makeKey(fuse::platform::PlatformEventType type, fuse::u32 keyCode) {
    fuse::platform::PlatformEvent event{};
    event.type = type;
    event.keyCode = keyCode;
    return event;
}

fuse::platform::PlatformEvent makeMouseMove(fuse::i32 x, fuse::i32 y) {
    fuse::platform::PlatformEvent event{};
    event.type = fuse::platform::PlatformEventType::MouseMove;
    event.mouseX = x;
    event.mouseY = y;
    return event;
}

fuse::platform::PlatformEvent makeRawMouseDelta(fuse::i32 dx, fuse::i32 dy) {
    fuse::platform::PlatformEvent event{};
    event.type = fuse::platform::PlatformEventType::RawMouseDelta;
    event.mouseX = dx;
    event.mouseY = dy;
    return event;
}

fuse::platform::PlatformEvent makeMouseButton(fuse::platform::PlatformEventType type, fuse::u8 button,
                                              fuse::i32 x, fuse::i32 y) {
    fuse::platform::PlatformEvent event{};
    event.type = type;
    event.mouseButton = button;
    event.mouseX = x;
    event.mouseY = y;
    return event;
}

void testKeyFromPlatformCodeMapsA() {
    expectTrue(fuse::platform::keyFromPlatformCode(65u) == fuse::platform::Key::A,
               "USB-ish 65 maps to Key::A");
    expectTrue(fuse::platform::keyFromPlatformCode(static_cast<fuse::u32>('A')) ==
                   fuse::platform::Key::A,
               "VK_A/'A' maps to Key::A");
    expectTrue(fuse::platform::keyFromPlatformCode(0u) == fuse::platform::Key::COUNT,
               "unknown code 0 is ignored");
}

void testKeyDownSetsDownAndPressedWithoutBeginFrame() {
    fuse::platform::InputState input;

    input.apply(makeKey(fuse::platform::PlatformEventType::KeyDown, 65u));

    expectTrue(input.keyDown(fuse::platform::Key::A), "KeyDown sets keyDown");
    expectTrue(input.keyPressed(fuse::platform::Key::A), "KeyDown sets keyPressed same frame");
    expectTrue(!input.keyReleased(fuse::platform::Key::A), "KeyDown does not set keyReleased");
}

void testBeginFrameClearsPressedHeldKeyStaysDown() {
    fuse::platform::InputState input;

    input.apply(makeKey(fuse::platform::PlatformEventType::KeyDown, 65u));
    input.beginFrame();

    expectTrue(!input.keyPressed(fuse::platform::Key::A), "beginFrame clears keyPressed");
    expectTrue(input.keyDown(fuse::platform::Key::A), "held key stays down after beginFrame");
    expectTrue(!input.keyReleased(fuse::platform::Key::A), "held key is not released");
}

void testKeyUpReleasedOneFrameThenCleared() {
    fuse::platform::InputState input;

    input.apply(makeKey(fuse::platform::PlatformEventType::KeyDown, 65u));
    input.beginFrame();
    input.apply(makeKey(fuse::platform::PlatformEventType::KeyUp, 65u));

    expectTrue(!input.keyDown(fuse::platform::Key::A), "KeyUp clears keyDown");
    expectTrue(input.keyReleased(fuse::platform::Key::A), "KeyUp sets keyReleased one frame");
    expectTrue(!input.keyPressed(fuse::platform::Key::A), "KeyUp does not keep keyPressed");

    input.beginFrame();

    expectTrue(!input.keyReleased(fuse::platform::Key::A), "beginFrame clears keyReleased");
    expectTrue(!input.keyDown(fuse::platform::Key::A), "released key stays up");
}

void testMouseMoveDeltaFromConsecutivePositions() {
    fuse::platform::InputState input;

    input.apply(makeMouseMove(10, 20));
    input.apply(makeMouseMove(15, 22));

    expectEqI32(input.mouseX(), 15, "second MouseMove x");
    expectEqI32(input.mouseY(), 22, "second MouseMove y");
    expectEqI32(input.mouseDeltaX(), 5, "delta x +5 from consecutive MouseMove");
    expectEqI32(input.mouseDeltaY(), 2, "delta y +2 from consecutive MouseMove");

    input.beginFrame();

    expectEqI32(input.mouseDeltaX(), 0, "beginFrame zeros mouse delta x");
    expectEqI32(input.mouseDeltaY(), 0, "beginFrame zeros mouse delta y");
    expectEqI32(input.mouseX(), 15, "beginFrame keeps mouse x");
    expectEqI32(input.mouseY(), 22, "beginFrame keeps mouse y");
}

void testLeftMouseButtonDownUp() {
    fuse::platform::InputState input;

    input.apply(makeMouseButton(fuse::platform::PlatformEventType::MouseButtonDown, 1u, 8, 12));
    expectTrue(input.mouseDown(fuse::platform::MouseButton::Left), "left MouseButtonDown");
    expectEqI32(input.mouseX(), 8, "button down x");
    expectEqI32(input.mouseY(), 12, "button down y");
    expectTrue(!input.mouseDown(fuse::platform::MouseButton::Right), "right stays up");

    input.apply(makeMouseButton(fuse::platform::PlatformEventType::MouseButtonUp, 1u, 8, 12));
    expectTrue(!input.mouseDown(fuse::platform::MouseButton::Left), "left MouseButtonUp");
}

void testUnknownKeyCodeIgnored() {
    fuse::platform::InputState input;

    input.apply(makeKey(fuse::platform::PlatformEventType::KeyDown, 0xFFFFu));

    expectTrue(!input.keyDown(fuse::platform::Key::A), "unknown KeyDown does not set A down");
    expectTrue(!input.keyPressed(fuse::platform::Key::A), "unknown KeyDown does not press A");
}

void testApplyPumpDrainsKeyMouseAndRawDelta() {
    fuse::platform::EventPump pump;
    fuse::platform::InputState input;

    pump.pushKeyDown(65u);
    pump.pushMouseMove(100, 50);

    fuse::platform::PlatformEvent raw{};
    raw.type = fuse::platform::PlatformEventType::RawMouseDelta;
    raw.mouseX = 7;
    raw.mouseY = -4;
    pump.pushSyntheticEvent(raw);

    const fuse::u32 applied = input.applyPump(pump);

    expectEqI32(static_cast<fuse::i32>(applied), 3, "applyPump applied KeyDown, MouseMove, RawMouseDelta");
    expectTrue(input.keyDown(fuse::platform::Key::A), "applyPump KeyDown 65 sets keyDown(A)");
    expectEqI32(input.mouseX(), 100, "applyPump MouseMove sets mouse x");
    expectEqI32(input.mouseY(), 50, "applyPump MouseMove sets mouse y");
    expectEqI32(input.mouseDeltaX(), 7, "applyPump raw delta x (first MouseMove does not delta)");
    expectEqI32(input.mouseDeltaY(), -4, "applyPump raw delta y");
    expectTrue(!pump.hasPendingEvents(), "applyPump drains the pump");
}

void testRawMouseDeltaAccumulatesWithoutChangingAbsolutePosition() {
    fuse::platform::InputState input;

    expectEqI32(input.mouseX(), 0, "default mouse x");
    expectEqI32(input.mouseY(), 0, "default mouse y");

    input.apply(makeRawMouseDelta(3, -1));
    input.apply(makeRawMouseDelta(1, 0));

    expectEqI32(input.mouseDeltaX(), 4, "raw delta x 3+1");
    expectEqI32(input.mouseDeltaY(), -1, "raw delta y -1+0");
    expectEqI32(input.mouseX(), 0, "RawMouseDelta does not overwrite mouse x");
    expectEqI32(input.mouseY(), 0, "RawMouseDelta does not overwrite mouse y");

    input.beginFrame();

    expectEqI32(input.mouseDeltaX(), 0, "beginFrame zeros raw mouse delta x");
    expectEqI32(input.mouseDeltaY(), 0, "beginFrame zeros raw mouse delta y");
    expectEqI32(input.mouseX(), 0, "beginFrame keeps mouse x after raw delta");
    expectEqI32(input.mouseY(), 0, "beginFrame keeps mouse y after raw delta");
}

void testCaptureReleaseRestoresCursorDeltas() {
    // Raw mode follows the window's capture: RawMouseDelta while captured (MouseMove only moves the
    // absolute position), cursor deltas from MouseMove again once capture is released.
    fuse::platform::EventPump pump;
    fuse::platform::Window window;
    fuse::platform::InputState input;

    pump.pushMouseMove(10, 10);
    window.setInputCapture(fuse::platform::InputCaptureMode::Captured, &pump);
    pump.pushSyntheticEvent(makeRawMouseDelta(5, 2));
    pump.pushMouseMove(40, 40); // accelerated OS cursor path: must not mix into the raw delta
    input.applyPump(pump);
    expectTrue(input.rawMouseActive(), "raw mode active while captured");
    expectEqI32(input.mouseDeltaX(), 5, "captured: delta x from raw input only");
    expectEqI32(input.mouseDeltaY(), 2, "captured: delta y from raw input only");
    expectEqI32(input.mouseX(), 40, "captured: MouseMove still updates absolute x");

    input.beginFrame();
    window.setInputCapture(fuse::platform::InputCaptureMode::Released, &pump);
    fuse::platform::PlatformEvent released{};
    expectTrue(pump.pollEvent(released) && released.type == fuse::platform::PlatformEventType::InputCaptureChanged &&
                   released.window == &window && !released.inputCaptured,
               "release enqueues InputCaptureChanged(false) for the window");
    input.apply(released);
    expectTrue(!input.rawMouseActive(), "raw mode off after capture release");

    pump.pushMouseMove(200, 100); // first move after release: new baseline (cursor was hidden/clipped)
    pump.pushMouseMove(203, 96);
    input.applyPump(pump);
    expectEqI32(input.mouseDeltaX(), 3, "released: MouseMove delta x restored (no jump from stale baseline)");
    expectEqI32(input.mouseDeltaY(), -4, "released: MouseMove delta y restored");
    expectEqI32(input.mouseX(), 203, "released: absolute x");

    // Recapture: raw mode resumes with the next raw delta.
    input.beginFrame();
    window.setInputCapture(fuse::platform::InputCaptureMode::Captured, &pump);
    pump.pushSyntheticEvent(makeRawMouseDelta(-1, 1));
    pump.pushMouseMove(250, 50);
    input.applyPump(pump);
    expectTrue(input.rawMouseActive(), "raw mode resumes after recapture");
    expectEqI32(input.mouseDeltaX(), -1, "recaptured: raw delta only");

    // Direct API (callers that track capture themselves).
    input.onInputCaptureChanged(false);
    expectTrue(!input.rawMouseActive(), "onInputCaptureChanged(false) leaves raw mode");
}

void testCaptureChangeWithoutPumpIsNoOpForUnchangedMode() {
    fuse::platform::EventPump pump;
    fuse::platform::Window window;
    window.setInputCapture(fuse::platform::InputCaptureMode::Released, &pump);
    expectTrue(!pump.hasPendingEvents(), "setting the current capture mode enqueues nothing");
    window.setInputCapture(fuse::platform::InputCaptureMode::Captured);
    expectTrue(!pump.hasPendingEvents(), "capture change without a pump does not touch other pumps");
    expectTrue(window.isInputCaptured(), "capture stored without a pump");
}

} // namespace

int main() {
    testKeyFromPlatformCodeMapsA();
    testKeyDownSetsDownAndPressedWithoutBeginFrame();
    testBeginFrameClearsPressedHeldKeyStaysDown();
    testKeyUpReleasedOneFrameThenCleared();
    testMouseMoveDeltaFromConsecutivePositions();
    testLeftMouseButtonDownUp();
    testUnknownKeyCodeIgnored();
    testApplyPumpDrainsKeyMouseAndRawDelta();
    testRawMouseDeltaAccumulatesWithoutChangingAbsolutePosition();
    testCaptureReleaseRestoresCursorDeltas();
    testCaptureChangeWithoutPumpIsNoOpForUnchangedMode();

    if (g_failures == 0) {
        std::printf("fuse_core input state: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core input state: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
