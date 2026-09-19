#include <fuse/platform/window_wsi.hpp>

#include <cstdlib>

namespace fuse::platform {

namespace {

bool hasDisplayServerEnv() {
#if defined(__linux__)
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return (display != nullptr && display[0] != '\0') ||
           (wayland != nullptr && wayland[0] != '\0');
#else
    return true;
#endif
}

} // namespace

WindowWsiKind activeWindowWsiKind() {
    return WindowWsiKind::Null;
}

const char* windowWsiBackendName() {
    return "null-wsi";
}

bool windowWsiAvailable() {
    return false;
}

bool displayServerAvailable() {
    return hasDisplayServerEnv();
}

void requiredVulkanInstanceExtensions(std::vector<const char*>& out) {
    out.clear();
}

bool createVulkanSurface(void* /*vkInstance*/, const Window& /*window*/, void** outSurface) {
    if (outSurface != nullptr) {
        *outSurface = nullptr;
    }
    return false;
}

} // namespace fuse::platform
