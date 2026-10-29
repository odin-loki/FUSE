#pragma once

// Internal native X11 (Xlib) window backend for fuse::platform::Window.
//
// This header deliberately does not include <X11/Xlib.h>: Xlib defines macros such as
// `None`, `KeyPress`, `FocusIn` and `Bool` that collide with FUSE enums. All Xlib usage
// lives in x11_window.cpp; callers only see opaque handles (Display* and Window XIDs are
// carried as void*).

#if defined(FUSE_PLATFORM_WINDOW_X11)

#include <fuse/types.hpp>

#include <vector>

namespace fuse::platform::x11 {

/// Lazily opens the process-wide X display (XOpenDisplay(nullptr)). Returns false when
/// DISPLAY is unset or the server is unreachable — headless path stays active.
bool ensureDisplay();

/// Opaque `Display*` (null when `ensureDisplay()` failed).
void* display();

/// Creates and maps a top-level window (WM_DELETE_WINDOW protocol, WM_NAME + _NET_WM_NAME).
/// Returns the X11 `Window` XID as an opaque pointer, or null on failure.
void* createWindow(u32 width, u32 height, const char* title);
void destroyWindow(void* window);
void setTitle(void* window, const char* title);
void resizeWindow(void* window, u32 width, u32 height);
/// Selects the backend's input mask on a foreign window attached via `setNativeHandleForPump`.
void selectInput(void* window);
void flush();

enum class EventKind : u8 {
    Ignored = 0,
    Close,
    Resize,
    FocusGained,
    FocusLost,
    KeyDown,
    KeyUp,
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    /// XInput2 XI_RawMotion (root window, device-level, pre-acceleration). `window` is null;
    /// `x`/`y` carry the signed integer device delta.
    RawMouseDelta,
    /// Committed text from the input method (XIM commit without a key, or a compose result). `text` is UTF-8.
    Text,
    /// IME pre-edit changed: `text` is the whole pre-edit (UTF-8, empty = ended), `caret` its byte caret.
    Composition,
};

struct Event {
    EventKind kind = EventKind::Ignored;
    void* window = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 keyCode = 0;  // Win32-style virtual key (see input.cpp `keyFromPlatformCode`)
    i32 x = 0;
    i32 y = 0;
    i32 wheelX = 0;   // MouseWheel: +/-120 per notch (Button6/7 horizontal, Button4/5 vertical)
    i32 wheelY = 0;
    u8 button = 0;    // 1=left 2=right 3=middle 4=X1 5=X2
    i32 caret = 0;    // Composition: caret byte offset into `text`
    char text[256] = {}; // KeyDown / Text: committed UTF-8 (Xutf8LookupString through the window's XIC, else
                         // XLookupString Latin-1); Composition: the pre-edit string
};

/// True when the server speaks XInput2 (>= 2.0) so XI_RawMotion can be selected. Probed once;
/// `FUSE_X11_NO_XI2=1` in the environment forces the fallback (cursor-delta MouseMove only).
bool rawMotionAvailable();

/// Selects (true) or deselects (false) XI_RawMotion on the root window for all master
/// pointers. Idempotent. Returns whether raw motion is selected afterwards (false when XI2 is
/// missing — callers keep the MotionNotify/cursor-delta path).
bool setRawMotionEnabled(bool enabled);

/// Pops one queued X event (non-blocking). Returns false when the queue is empty.
bool pollEvent(Event& out);

// --- UI1 text services (XIM + CLIPBOARD selection) ---------------------------------------------
//
// Each window gets an input context when an input method opens (XOpenIM under the process locale and XMODIFIERS;
// the app must have called setlocale(LC_CTYPE, "") or similar for a non-C IM). Over-the-spot style with pre-edit
// callbacks is preferred, so the pre-edit is drawn by FUSE UI; root/nothing styles fall back to the IM's own window.

/// True when an XIM is open (and so windows get input contexts).
bool inputMethodAvailable();
/// Focuses / unfocuses the window's input context (beginTextInput / endTextInput).
void setTextInputFocus(void* window, bool focused);
/// Spot location for the IME candidate / over-the-spot window (client pixels, baseline-ish point).
void setTextInputSpot(void* window, i32 x, i32 y);
/// Test hook: runs the pre-edit draw callback logic on `window` as if the IM sent it (chars replaced at `first`,
/// `length` chars, with `utf8` and `caret` in characters); queues a Composition event like the real callback.
void debugPreeditDraw(void* window, i32 first, i32 length, const char* utf8, i32 caret);

/// CLIPBOARD selection: owns it with a hidden helper window and serves UTF8_STRING / STRING / TEXT / TARGETS
/// requests from pollEvent. False without a display.
bool setClipboard(const char* utf8, usize length);
/// Reads the CLIPBOARD (own copy, or XConvertSelection to UTF8_STRING then STRING, waiting up to `timeoutMs`).
/// False when empty, unowned, timed out or INCR-sized (> max request; not supported).
bool clipboard(std::vector<char>& out, u32 timeoutMs = 500u);
/// Serves pending selection requests without a pump (pollEvent also does this).
void serviceSelections();

/// Maps an X11 KeySym to the Win32-style virtual key FUSE uses as `PlatformEvent::keyCode`.
u32 keysymToVirtualKey(unsigned long keysym);

/// VK_KHR_surface + VK_KHR_xlib_surface (only when the Vulkan loader was found at configure).
void vulkanInstanceExtensions(std::vector<const char*>& out);
bool createVulkanSurface(void* vkInstance, void* window, void** outSurface);

} // namespace fuse::platform::x11

#endif // FUSE_PLATFORM_WINDOW_X11
