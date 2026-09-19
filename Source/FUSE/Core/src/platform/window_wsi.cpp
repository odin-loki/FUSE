#include <fuse/platform/window_wsi.hpp>

namespace fuse::platform {

WindowWsiKind activeWindowWsiKind() {
    return WindowWsiKind::Null;
}

const char* windowWsiBackendName() {
    return "null-wsi";
}

bool windowWsiAvailable() {
    return false;
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
