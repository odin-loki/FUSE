#pragma once

#include <fuse/types.hpp>

#include <string_view>

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

/// Stable text names (action-map files, Lua `Input.key_held("W")`): "A".."Z", "0".."9",
/// "F1".."F12", "Space", "Enter", "Escape", "Tab", "Backspace", "Delete", "Insert", "Left",
/// "Right", "Up", "Down", "PageUp", "PageDown", "Home", "End", "Ctrl", "Shift", "Alt", "Super",
/// "CapsLock". Lookup is case-insensitive; unknown names return `Key::COUNT`.
std::string_view keyName(Key key);
Key keyFromName(std::string_view name);
/// "Left", "Right", "Middle", "X1", "X2" (case-insensitive); unknown -> `MouseButton::COUNT`.
std::string_view mouseButtonName(MouseButton button);
MouseButton mouseButtonFromName(std::string_view name);

/// Frame input snapshot. No heap: down/pressed/released are fixed arrays.
///
/// Call `beginFrame()` once per tick, then `apply()` each polled event
/// (or `applyPump()` to drain an `EventPump` — that drain does not call `beginFrame()`).
/// `keyPressed` is true only until the next `beginFrame()` after a `KeyDown`.
class InputState {
public:
    /// Clears pressed-this-frame, released-this-frame (keys and mouse buttons), and mouse delta.
    /// Held keys and absolute mouse position stay.
    void beginFrame();

    /// Apply one `KeyDown`/`KeyUp`, `MouseMove`, `RawMouseDelta`, or `MouseButtonDown`/`Up`.
    /// `RawMouseDelta` accumulates `mouseDeltaX/Y` and does not overwrite absolute `mouseX/Y`.
    /// Once a raw delta has been seen, `MouseMove` only updates the absolute position: the OS
    /// cursor path is acceleration-scaled, so mixing it in would corrupt the raw delta.
    /// `InputCaptureChanged` with `inputCaptured == false` (capture released: raw input stops)
    /// switches back to `MouseMove` deltas — see `onInputCaptureChanged`.
    /// Other event types are ignored. Unknown key/button codes are ignored.
    void apply(const PlatformEvent& event);

    /// Follow the window's capture state. Release leaves raw mode (`rawMouseActive()` false) and
    /// drops the stale cursor baseline, so the next `MouseMove` sets the position and the ones
    /// after it produce deltas. Capture changes nothing until the first `RawMouseDelta`.
    void onInputCaptureChanged(bool captured);

    /// Poll every pending event from `pump` and `apply()` each. Does not call `beginFrame()`.
    /// Returns the number of events applied (including types `apply` ignores).
    u32 applyPump(EventPump& pump);

    bool keyDown(Key key) const;
    bool keyPressed(Key key) const;
    bool keyReleased(Key key) const;
    bool mouseDown(MouseButton button) const;
    /// Button went down / up since the last `beginFrame()`.
    bool mousePressed(MouseButton button) const;
    bool mouseReleased(MouseButton button) const;

    /// Focus loss: every held key and button is released (released-this-frame set for each).
    void releaseAll();

    i32 mouseX() const { return m_mouseX; }
    i32 mouseY() const { return m_mouseY; }
    i32 mouseDeltaX() const { return m_mouseDeltaX; }
    i32 mouseDeltaY() const { return m_mouseDeltaY; }

    /// True from the first `RawMouseDelta` until capture is released: deltas then come from raw
    /// input only.
    bool rawMouseActive() const { return m_rawMouseActive; }

private:
    static constexpr u32 kKeyCount = static_cast<u32>(Key::COUNT);
    static constexpr u32 kMouseButtonCount = static_cast<u32>(MouseButton::COUNT);

    bool m_keyDown[kKeyCount]{};
    bool m_keyPressed[kKeyCount]{};
    bool m_keyReleased[kKeyCount]{};
    bool m_mouseDown[kMouseButtonCount]{};
    bool m_mousePressed[kMouseButtonCount]{};
    bool m_mouseReleased[kMouseButtonCount]{};
    i32 m_mouseX = 0;
    i32 m_mouseY = 0;
    i32 m_mouseDeltaX = 0;
    i32 m_mouseDeltaY = 0;
    bool m_haveMousePosition = false;
    bool m_rawMouseActive = false;
};

} // namespace fuse::platform
