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
};

struct Event {
    EventKind kind = EventKind::Ignored;
    void* window = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 keyCode = 0;  // Win32-style virtual key (see input.cpp `keyFromPlatformCode`)
    i32 x = 0;
    i32 y = 0;        // wheel delta (+/-120) for MouseWheel
    u8 button = 0;    // 1=left 2=right 3=middle 4=X1 5=X2
};

/// Pops one queued X event (non-blocking). Returns false when the queue is empty.
bool pollEvent(Event& out);

/// Maps an X11 KeySym to the Win32-style virtual key FUSE uses as `PlatformEvent::keyCode`.
u32 keysymToVirtualKey(unsigned long keysym);

/// VK_KHR_surface + VK_KHR_xlib_surface (only when the Vulkan loader was found at configure).
void vulkanInstanceExtensions(std::vector<const char*>& out);
bool createVulkanSurface(void* vkInstance, void* window, void** outSurface);

} // namespace fuse::platform::x11

#endif // FUSE_PLATFORM_WINDOW_X11
