#if defined(FUSE_HAS_GLFW_WINDOW)

#include <fuse/platform/window.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace fuse::platform {

namespace {

bool g_glfwInitialized = false;

} // namespace

std::unique_ptr<PlatformWindow> createGlfwWindow(const WindowDesc& desc) {
    WindowInfo info{};
    info.backend = WindowBackend::Glfw;

    if (!g_glfwInitialized) {
        if (glfwInit() != GLFW_TRUE) {
            info.message = "glfwInit failed — falling back to invalid GLFW window";
            return std::unique_ptr<PlatformWindow>(new PlatformWindow(info, nullptr));
        }
        g_glfwInitialized = true;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);

    GLFWwindow* glfwWindow =
        glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height),
                         desc.title != nullptr ? desc.title : "FUSE", nullptr, nullptr);
    if (glfwWindow == nullptr) {
        info.message = "glfwCreateWindow failed — no native window handle";
        return std::unique_ptr<PlatformWindow>(new PlatformWindow(info, nullptr));
    }

    info.valid = true;
    info.width = desc.width;
    info.height = desc.height;
    info.message = "GLFW window created (WSI surface via VulkanPresentable)";
    return std::unique_ptr<PlatformWindow>(new PlatformWindow(info, glfwWindow));
}

} // namespace fuse::platform

#endif // defined(FUSE_HAS_GLFW_WINDOW)
