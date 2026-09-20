#pragma once

#include <fuse/types.hpp>

namespace fuse::platform {

class EventPump;
struct PlatformEvent;

enum class Key : u16 {
    A = 0,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Space,
    Enter,
    Escape,
    Tab,
    Backspace,
    Delete,
    Insert,
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
    Ctrl,
    Shift,
    Alt,
    Super,
    CapsLock,
    COUNT
};

enum class MouseButton : u8 { Left, Right, Middle, X1, X2, COUNT };

/// Map a platform `keyCode` (Win32 virtual-key / ASCII / USB-ish) to `Key`.
/// Unknown codes return `Key::COUNT` and are ignored by `InputState::apply`.
Key keyFromPlatformCode(u32 keyCode);

/// Frame input snapshot. No heap: down/pressed/released are fixed arrays.
///
/// Call `beginFrame()` once per tick, then `apply()` each polled event
/// (or `applyPump()` to drain an `EventPump` — that drain does not call `beginFrame()`).
/// `keyPressed` is true only until the next `beginFrame()` after a `KeyDown`.
class InputState {
public:
    /// Clears pressed-this-frame, released-this-frame, and mouse delta.
    /// Held keys and absolute mouse position stay.
    void beginFrame();

    /// Apply one `KeyDown`/`KeyUp`, `MouseMove`, `RawMouseDelta`, or `MouseButtonDown`/`Up`.
    /// `RawMouseDelta` accumulates `mouseDeltaX/Y` and does not overwrite absolute `mouseX/Y`.
    /// Other event types are ignored. Unknown key/button codes are ignored.
    void apply(const PlatformEvent& event);

    /// Poll every pending event from `pump` and `apply()` each. Does not call `beginFrame()`.
    /// Returns the number of events applied (including types `apply` ignores).
    u32 applyPump(EventPump& pump);

    bool keyDown(Key key) const;
    bool keyPressed(Key key) const;
    bool keyReleased(Key key) const;
    bool mouseDown(MouseButton button) const;

    i32 mouseX() const { return m_mouseX; }
    i32 mouseY() const { return m_mouseY; }
    i32 mouseDeltaX() const { return m_mouseDeltaX; }
    i32 mouseDeltaY() const { return m_mouseDeltaY; }

private:
    static constexpr u32 kKeyCount = static_cast<u32>(Key::COUNT);
    static constexpr u32 kMouseButtonCount = static_cast<u32>(MouseButton::COUNT);

    bool m_keyDown[kKeyCount]{};
    bool m_keyPressed[kKeyCount]{};
    bool m_keyReleased[kKeyCount]{};
    bool m_mouseDown[kMouseButtonCount]{};
    i32 m_mouseX = 0;
    i32 m_mouseY = 0;
    i32 m_mouseDeltaX = 0;
    i32 m_mouseDeltaY = 0;
    bool m_haveMousePosition = false;
};

} // namespace fuse::platform
