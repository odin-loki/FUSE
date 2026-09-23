// B1.7 / B1.8 platform gates (FUSE_MASTER_PLAN "B1.8 — Phase 1 Deliverables & Test Suite"):
//   - Input system reports key_pressed for exactly one frame on a keydown event (including OS
//     auto-repeat KeyDowns while held and a press+release inside one frame)
//   - Raw mouse delta is unaffected by OS cursor acceleration: once raw input is flowing,
//     accelerated MouseMove positions never leak into the delta
// Opening a real window, surface creation and real OS acceleration settings are hardware/manual.

#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input.hpp>

#include <cstdio>
#include <cstdlib>
#include <random>

namespace {

using fuse::i32;
using fuse::u32;
using fuse::platform::InputState;
using fuse::platform::Key;
using fuse::platform::PlatformEvent;
using fuse::platform::PlatformEventType;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

PlatformEvent keyEvent(PlatformEventType type, u32 code) {
    PlatformEvent e{};
    e.type = type;
    e.keyCode = code;
    return e;
}

PlatformEvent mouseEvent(PlatformEventType type, i32 x, i32 y) {
    PlatformEvent e{};
    e.type = type;
    e.mouseX = x;
    e.mouseY = y;
    return e;
}

void testKeyPressedExactlyOneFrame() {
    std::mt19937 rng(0xB17u);
    constexpr u32 kCodeA = 65u;
    u32 violations = 0;
    for (u32 trial = 0; trial < 2000u; ++trial) {
        InputState input;
        const u32 idleFrames = rng() % 4u;
        const u32 heldFrames = 1u + rng() % 6u;
        const u32 repeatsPerFrame = rng() % 3u;
        u32 pressedFrames = 0;

        for (u32 f = 0; f < idleFrames; ++f) {
            input.beginFrame();
            pressedFrames += input.keyPressed(Key::A) ? 1u : 0u;
        }
        input.beginFrame();
        input.apply(keyEvent(PlatformEventType::KeyDown, kCodeA));
        pressedFrames += input.keyPressed(Key::A) ? 1u : 0u;
        for (u32 f = 1; f < heldFrames; ++f) {
            input.beginFrame();
            for (u32 r = 0; r < repeatsPerFrame; ++r) {
                input.apply(keyEvent(PlatformEventType::KeyDown, kCodeA)); // OS auto-repeat
            }
            pressedFrames += input.keyPressed(Key::A) ? 1u : 0u;
            violations += input.keyDown(Key::A) ? 0u : 1u;
        }
        input.beginFrame();
        input.apply(keyEvent(PlatformEventType::KeyUp, kCodeA));
        pressedFrames += input.keyPressed(Key::A) ? 1u : 0u;
        violations += input.keyReleased(Key::A) ? 0u : 1u;
        input.beginFrame();
        pressedFrames += input.keyPressed(Key::A) ? 1u : 0u;
        violations += pressedFrames == 1u ? 0u : 1u;
    }
    expectTrue(violations == 0u, "keyPressed is true for exactly one frame per keydown (auto-repeat ignored)");

    InputState tap;
    tap.beginFrame();
    tap.apply(keyEvent(PlatformEventType::KeyDown, kCodeA));
    tap.apply(keyEvent(PlatformEventType::KeyUp, kCodeA));
    expectTrue(tap.keyPressed(Key::A) && tap.keyReleased(Key::A) && !tap.keyDown(Key::A),
               "press+release inside one frame still reports one pressed frame");
    tap.beginFrame();
    expectTrue(!tap.keyPressed(Key::A) && !tap.keyReleased(Key::A), "tap edges clear next frame");
}

void testRawMouseDeltaIgnoresAcceleratedCursor() {
    std::mt19937 rng(0xACCu);
    std::uniform_int_distribution<i32> rawStep(-20, 20);
    InputState input;
    // Simulate a Win32-style stream: each device report arrives as WM_INPUT (raw counts) and the
    // OS also moves the cursor by an accelerated amount (WM_MOUSEMOVE with absolute positions).
    i32 cursorX = 400;
    i32 cursorY = 300;
    input.apply(mouseEvent(PlatformEventType::MouseMove, cursorX, cursorY));
    u32 mismatchedFrames = 0;
    for (u32 frame = 0; frame < 1000u; ++frame) {
        input.beginFrame();
        i32 rawX = 0;
        i32 rawY = 0;
        const u32 reports = 1u + rng() % 8u;
        for (u32 r = 0; r < reports; ++r) {
            const i32 dx = rawStep(rng);
            const i32 dy = rawStep(rng);
            rawX += dx;
            rawY += dy;
            input.apply(mouseEvent(PlatformEventType::RawMouseDelta, dx, dy));
            // Enhance-pointer-precision style acceleration on the cursor path.
            const i32 gain = (std::abs(dx) + std::abs(dy)) > 10 ? 3 : 1;
            cursorX += dx * gain;
            cursorY += dy * gain;
            input.apply(mouseEvent(PlatformEventType::MouseMove, cursorX, cursorY));
        }
        mismatchedFrames += (input.mouseDeltaX() == rawX && input.mouseDeltaY() == rawY) ? 0u : 1u;
    }
    expectTrue(input.rawMouseActive(), "raw input detected");
    expectTrue(mismatchedFrames == 0u, "per-frame mouse delta equals the raw device counts exactly");
    expectTrue(input.mouseX() == cursorX && input.mouseY() == cursorY,
               "absolute cursor position still tracks MouseMove");

    // Without raw input (e.g. platforms lacking it) the position delta remains the fallback.
    InputState fallback;
    fallback.apply(mouseEvent(PlatformEventType::MouseMove, 10, 10));
    fallback.beginFrame();
    fallback.apply(mouseEvent(PlatformEventType::MouseMove, 13, 6));
    expectTrue(!fallback.rawMouseActive() && fallback.mouseDeltaX() == 3 && fallback.mouseDeltaY() == -4,
               "cursor-position delta is used only when no raw input exists");
}

} // namespace

int main() {
    testKeyPressedExactlyOneFrame();
    testRawMouseDeltaIgnoresAcceleratedCursor();

    if (g_failures == 0) {
        std::printf("fuse_core_b1_platform_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core_b1_platform_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
