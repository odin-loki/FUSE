#pragma once

#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::platform {

/// Desktop window backend for WSI surface creation (B2.2 follow-up).
enum class WindowBackend : u8 {
    /// No OS window — CI / headless default.
    Null = 0,
    /// GLFW hidden window when `FUSE_PLATFORM_WINDOW_GLFW` is enabled at build time.
    Glfw = 1,
};

struct WindowDesc {
    WindowBackend backend = WindowBackend::Null;
    u32 width = 1280;
    u32 height = 720;
    const char* title = "FUSE";
    /// When false, GLFW creates a hidden window suitable for off-screen WSI bootstrap.
    bool visible = false;
};

struct WindowInfo {
    WindowBackend backend = WindowBackend::Null;
    bool valid = false;
    u32 width = 0;
    u32 height = 0;
    std::string message;
};

/// Thin OS window abstraction — WSI surface creation stays in Hybrid presentable glue.
class PlatformWindow {
public:
    static std::unique_ptr<PlatformWindow> create(const WindowDesc& desc);

    const WindowInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }

    /// Opaque native window handle (`GLFWwindow*` when GLFW backend is active).
    void* nativeHandle() const { return m_nativeHandle; }

private:
    friend std::unique_ptr<PlatformWindow> createGlfwWindow(const WindowDesc& desc);
    PlatformWindow(WindowInfo info, void* nativeHandle);

    WindowInfo m_info;
    void* m_nativeHandle = nullptr;
};

} // namespace fuse::platform
