#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input.hpp>

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

} // namespace

int main() {
    testKeyFromPlatformCodeMapsA();
    testKeyDownSetsDownAndPressedWithoutBeginFrame();
    testBeginFrameClearsPressedHeldKeyStaysDown();
    testKeyUpReleasedOneFrameThenCleared();
    testMouseMoveDeltaFromConsecutivePositions();
    testLeftMouseButtonDownUp();
    testUnknownKeyCodeIgnored();
    testRawMouseDeltaAccumulatesWithoutChangingAbsolutePosition();

    if (g_failures == 0) {
        std::printf("fuse_core input state: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core input state: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
