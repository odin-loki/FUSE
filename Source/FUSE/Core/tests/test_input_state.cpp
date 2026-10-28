#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/platform/input_chord.hpp>
#include <fuse/platform/key_index.hpp>
#include <fuse/platform/radial_deadzone.hpp>
#include <fuse/platform/repeat_delay.hpp>
#include <fuse/platform/window.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

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

void testChordMatcherWithinGapLateAndReset() {
    constexpr fuse::u32 kGapMs = 100u;

    fuse::platform::ChordMatcher within({fuse::platform::Key::A, fuse::platform::Key::B}, kGapMs);
    within.feed(fuse::platform::Key::A, 0u);
    expectTrue(!within.matched(), "A alone does not complete the chord");
    within.feed(fuse::platform::Key::B, 40u);
    expectTrue(within.matched(), "A then B within the gap matches");
    within.feed(fuse::platform::Key::A, 50u);
    expectTrue(!within.matched(), "the next feed clears matched and starts fresh");
    within.feed(fuse::platform::Key::B, 80u);
    expectTrue(within.matched(), "A then B matches again after a completed chord");

    fuse::platform::ChordMatcher onTime({fuse::platform::Key::A, fuse::platform::Key::B}, kGapMs);
    onTime.feed(fuse::platform::Key::A, 0u);
    onTime.feed(fuse::platform::Key::B, kGapMs);
    expectTrue(onTime.matched(), "a gap equal to maxGapMs still matches");

    fuse::platform::ChordMatcher late({fuse::platform::Key::A, fuse::platform::Key::B}, kGapMs);
    late.feed(fuse::platform::Key::A, 0u);
    late.feed(fuse::platform::Key::B, kGapMs + 1u);
    expectTrue(!late.matched(), "A then a late B does not match");

    fuse::platform::ChordMatcher restarted({fuse::platform::Key::A, fuse::platform::Key::B}, kGapMs);
    restarted.feed(fuse::platform::Key::A, 0u);
    restarted.feed(fuse::platform::Key::A, 1000u);
    expectTrue(!restarted.matched(), "a late opener restarts and does not complete");
    restarted.feed(fuse::platform::Key::B, 1040u);
    expectTrue(restarted.matched(), "B within the gap of a restarted A matches");

    fuse::platform::ChordMatcher wrong({fuse::platform::Key::A, fuse::platform::Key::B}, kGapMs);
    wrong.feed(fuse::platform::Key::A, 0u);
    wrong.feed(fuse::platform::Key::C, 10u);
    expectTrue(!wrong.matched(), "a wrong key is not a match");
    wrong.feed(fuse::platform::Key::B, 20u);
    expectTrue(!wrong.matched(), "a wrong key resets");
    wrong.feed(fuse::platform::Key::A, 30u);
    wrong.feed(fuse::platform::Key::B, 40u);
    expectTrue(wrong.matched(), "A then B matches after the reset");

    fuse::platform::ChordMatcher repeated({fuse::platform::Key::A, fuse::platform::Key::B}, kGapMs);
    repeated.feed(fuse::platform::Key::A, 0u);
    repeated.feed(fuse::platform::Key::A, 10u);
    repeated.feed(fuse::platform::Key::B, 20u);
    expectTrue(!repeated.matched(), "a wrong opener is discarded and does not restart the chord");

    fuse::platform::ChordMatcher empty({}, kGapMs);
    empty.feed(fuse::platform::Key::A, 0u);
    empty.feed(fuse::platform::Key::B, 10u);
    expectTrue(!empty.matched(), "empty sequence never matches");
}

void testRadialDeadzone() {
    const auto near = [](float actual, float expected) {
        return std::fabs(actual - expected) <= 1e-5f;
    };

    const fuse::platform::StickAxis full = fuse::platform::apply_radial_deadzone(1.f, 0.f, 0.2f);
    expectTrue(near(full.x, 1.f) && near(full.y, 0.f), "(1, 0) with deadzone 0.2 stays (1, 0)");

    const fuse::platform::StickAxis inside = fuse::platform::apply_radial_deadzone(0.1f, 0.f, 0.2f);
    expectTrue(inside.x == 0.f && inside.y == 0.f, "(0.1, 0) with deadzone 0.2 is (0, 0)");

    const fuse::platform::StickAxis mid = fuse::platform::apply_radial_deadzone(0.6f, 0.f, 0.2f);
    expectTrue(near(mid.x, 0.5f) && near(mid.y, 0.f), "(0.6, 0) with deadzone 0.2 is (0.5, 0)");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const fuse::platform::StickAxis badX = fuse::platform::apply_radial_deadzone(nan, 0.f, 0.2f);
    const fuse::platform::StickAxis badY = fuse::platform::apply_radial_deadzone(0.f, inf, 0.2f);
    const fuse::platform::StickAxis badDz = fuse::platform::apply_radial_deadzone(1.f, 0.f, nan);
    expectTrue(badX.x == 0.f && badX.y == 0.f, "non-finite x returns (0, 0)");
    expectTrue(badY.x == 0.f && badY.y == 0.f, "non-finite y returns (0, 0)");
    expectTrue(badDz.x == 0.f && badDz.y == 0.f, "non-finite deadzone returns (0, 0)");

    const fuse::platform::StickAxis negativeDeadzone =
        fuse::platform::apply_radial_deadzone(0.4f, 0.f, -1.f);
    expectTrue(near(negativeDeadzone.x, 0.4f) && near(negativeDeadzone.y, 0.f),
               "negative deadzone clamps to 0");

    const fuse::platform::StickAxis highDeadzone =
        fuse::platform::apply_radial_deadzone(0.5f, 0.f, 1.5f);
    expectTrue(highDeadzone.x == 0.f && highDeadzone.y == 0.f,
               "deadzone above 0.99 clamps and rejects length 0.5");

    const fuse::platform::StickAxis pastUnit =
        fuse::platform::apply_radial_deadzone(2.f, 0.f, 0.2f);
    expectTrue(near(pastUnit.x, 1.f) && near(pastUnit.y, 0.f),
               "length > 1 scales onto the unit circle");

    const fuse::platform::StickAxis zero = fuse::platform::apply_radial_deadzone(0.f, 0.f, 0.2f);
    expectTrue(zero.x == 0.f && zero.y == 0.f, "zero length returns (0, 0)");

    const fuse::platform::StickAxis up = fuse::platform::apply_radial_deadzone(0.f, 0.6f, 0.2f);
    expectTrue(near(up.x, 0.f) && near(up.y, 0.5f), "(0, 0.6) with deadzone 0.2 is (0, 0.5)");

    const fuse::platform::StickAxis left = fuse::platform::apply_radial_deadzone(-0.6f, 0.f, 0.2f);
    expectTrue(near(left.x, -0.5f) && near(left.y, 0.f), "(-0.6, 0) with deadzone 0.2 is (-0.5, 0)");
}

// No allocation. A failure returns from main before later tests.
void testKeyIndexOk() {
    expectTrue(fuse::platform::key_index_ok(0), "key index 0 is ok");
    expectTrue(fuse::platform::key_index_ok(255), "key index 255 is ok");
    expectTrue(!fuse::platform::key_index_ok(-1), "key index -1 is not ok");
    expectTrue(!fuse::platform::key_index_ok(256), "key index 256 is not ok");
}

void testRepeatDelayOk() {
    expectTrue(fuse::platform::repeat_delay_ok(0), "repeat delay 0 is ok");
    expectTrue(fuse::platform::repeat_delay_ok(400), "repeat delay 400 is ok");
    expectTrue(fuse::platform::repeat_delay_ok(2000), "repeat delay 2000 is ok");
    expectTrue(!fuse::platform::repeat_delay_ok(-1), "repeat delay -1 is not ok");
    expectTrue(!fuse::platform::repeat_delay_ok(2001), "repeat delay 2001 is not ok");
}

} // namespace

int main() {
    testKeyIndexOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core input state: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testRepeatDelayOk();
    testRadialDeadzone();
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
    testChordMatcherWithinGapLateAndReset();

    if (g_failures == 0) {
        std::printf("fuse_core input state: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core input state: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
