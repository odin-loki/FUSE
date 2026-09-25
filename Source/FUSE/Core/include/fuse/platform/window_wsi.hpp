#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::platform {

class Window;

/// Active desktop WSI backend for `fuse::platform::Window`.
enum class WindowWsiKind : u8 {
    Null = 0,
    Glfw = 1,
    Win32 = 2,
    /// Native Xlib window + VK_KHR_xlib_surface (Linux/BSD fallback when GLFW is absent).
    X11 = 3,
};

/// Returns the compile-time selected WSI backend (Null when GLFW option is OFF).
WindowWsiKind activeWindowWsiKind();

/// Human-readable backend label for diagnostics/tests.
const char* windowWsiBackendName();

/// True when a real desktop WSI backend is compiled in and initialized successfully.
bool windowWsiAvailable();

/// True when a display server is available (DISPLAY/WAYLAND on Linux; always true elsewhere).
bool displayServerAvailable();

/// Instance extensions required before `vkCreateInstance` (empty for Null WSI).
void requiredVulkanInstanceExtensions(std::vector<const char*>& out);

/// Opaque native display connection (`Display*` on the X11 backend); null on other
/// backends or when no display server is reachable.
void* nativeDisplayHandle();

/// True when captured windows receive unaccelerated `PlatformEventType::RawMouseDelta` events
/// from the OS (Win32 WM_INPUT; X11 XInput2 XI_RawMotion). False means the backend falls back
/// to cursor-position deltas (`MouseMove`), which include the OS pointer acceleration.
bool rawMouseInputAvailable();

/// Creates `VkSurfaceKHR` from a platform window. Returns false on Null WSI / headless CI.
bool createVulkanSurface(void* vkInstance, const Window& window, void** outSurface);

} // namespace fuse::platform
