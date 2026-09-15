#include <fuse/platform/window.hpp>

namespace fuse::platform {

PlatformWindow::PlatformWindow(WindowInfo info, void* nativeHandle)
    : m_info(info), m_nativeHandle(nativeHandle) {}

std::unique_ptr<PlatformWindow> PlatformWindow::create(const WindowDesc& desc) {
    if (desc.backend == WindowBackend::Glfw) {
#if defined(FUSE_HAS_GLFW_WINDOW)
        return createGlfwWindow(desc);
#else
        WindowInfo info{};
        info.backend = WindowBackend::Null;
        info.valid = true;
        info.width = desc.width;
        info.height = desc.height;
        info.message =
            "GLFW window backend requested but FUSE_PLATFORM_WINDOW_GLFW is OFF — using null window";
        return std::unique_ptr<PlatformWindow>(new PlatformWindow(info, nullptr));
#endif
    }

    WindowInfo info{};
    info.backend = WindowBackend::Null;
    info.valid = true;
    info.width = desc.width;
    info.height = desc.height;
    info.message = "Null window backend — no OS surface";
    return std::unique_ptr<PlatformWindow>(new PlatformWindow(info, nullptr));
}

} // namespace fuse::platform
