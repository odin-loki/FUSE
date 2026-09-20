#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::platform {

class EventPump;

/// Game-path window description (master plan §B1.7 / P1 §1.7).
struct WindowDesc {
    const char* title = "FUSE";
    u32 width = 1920;
    u32 height = 1080;
    bool fullscreen = false;
    bool borderless = false;
    bool vsync = true;
    /// When true, construct an owned native window (hidden Win32 HWND / GLFW).
    /// Default and headless construction keep `nativeHandle().value == nullptr`.
    bool createNative = false;
};

/// Opaque native window token (HWND, X11 Window, NSWindow*, ANativeWindow*, etc.).
struct NativeWindowHandle {
    void* value = nullptr;
};

enum class WindowCloseRequest {
    None,
    Requested,
};

/// Game-path focus/capture (master plan A6). Qt editor input stays independent
/// until the game window is `Captured`. Headless default is `Released`.
enum class InputCaptureMode : u8 {
    Released,
    Captured,
};

/// Metadata for wiring a platform window into the Vulkan RHI external-surface path.
///
/// `presentable` is true when a native window handle exists. `nativeSurface` stays
/// nullptr until WSI creates an opaque `VkSurfaceKHR`.
struct VulkanSurfaceWire {
    void* nativeSurface = nullptr;
    bool presentable = false;
};

/// Portable game window. Default / headless construction stays native-handle-less.
/// Opt-in owned HWND (hidden overlapped, not message-only) via `WindowDesc::createNative`.
/// Editor viewport windowing remains Qt-owned (Track A / B6).
class Window {
public:
    Window();
    explicit Window(const WindowDesc& desc);
    ~Window();

    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool isValid() const { return m_valid; }

    WindowDesc description() const;
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    bool isFullscreen() const { return m_fullscreen; }
    bool vsyncEnabled() const { return m_vsync; }
    bool isFocused() const { return m_focused; }

    /// Stores capture mode. Headless (null HWND) is a no-op for OS clip/cursor/raw input.
    /// Win32 owned HWND: Captured clips the cursor, hides it once, and registers RID_INPUT
    /// mouse; Released / destructor restores clip/cursor and unregisters raw input.
    void setInputCapture(InputCaptureMode mode);
    InputCaptureMode inputCapture() const { return m_inputCapture; }
    bool isInputCaptured() const { return m_inputCapture == InputCaptureMode::Captured; }

    NativeWindowHandle nativeHandle() const;

    /// Test / embedding hook — attach an existing native window (HWND on Win32)
    /// so `EventPump::processOsEvents` can drain its message queue.
    ///
    /// Does not create or take ownership of the handle. Pass null to detach.
    /// The default constructor never creates a Win32 window.
    void setNativeHandleForPump(void* hwnd);

    /// Opaque `VkSurfaceKHR` when WSI is wired; null until WSI creates the surface.
    void* nativeVulkanSurface() const;

    /// Maps this window to renderer surface metadata (see TRACK-B-VULKAN §Surface abstraction).
    VulkanSurfaceWire vulkanSurfaceWire() const;

    void setTitle(const char* title);
    /// When `pump` is non-null and geometry changes, enqueues `WindowResized`.
    void resize(u32 width, u32 height, EventPump* pump = nullptr);
    void setFullscreen(bool fullscreen);

    /// When `pump` is non-null, enqueues `WindowFocusGained` / `WindowFocusLost` on change.
    void setFocused(bool focused, EventPump* pump = nullptr);

    /// When `pump` is non-null, enqueues `WindowCloseRequested` once per close cycle.
    void requestClose(EventPump* pump = nullptr);
    WindowCloseRequest closeRequest() const { return m_closeRequest; }
    void clearCloseRequest();

private:
    friend class EventPump;

    bool m_valid = false;
    u32 m_width = 0;
    u32 m_height = 0;
    bool m_fullscreen = false;
    bool m_borderless = false;
    bool m_vsync = true;
    bool m_createNative = false;
    bool m_focused = true;
    InputCaptureMode m_inputCapture = InputCaptureMode::Released;
    WindowCloseRequest m_closeRequest = WindowCloseRequest::None;
    std::string m_title = "FUSE";
    void* m_nativeWindow = nullptr;
    bool m_ownsNativeWindow = false;
    bool m_pumpAsHwnd = false;
};

} // namespace fuse::platform
