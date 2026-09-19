#pragma once

#include <fuse/types.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/vk/surface.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#endif

namespace fuse::editor {

/// U6 → Track B bridge: Qt/editor viewport surface queued for `SwapchainDesc` wiring.
struct ViewportSwapchainHandoff {
    void* nativeSurface = nullptr;
    u32 width = 0;
    u32 height = 0;
    bool pending = false;
    bool consumed = false;
    bool qtStubSurface = false;
    bool qtRealSurface = false;
    void* qtVkInstance = nullptr;
    const char* handoffSource = nullptr;
#if defined(FUSE_VULKAN_BACKEND)
    fuse::renderer::SurfaceDesc surface{};
    fuse::renderer::SwapchainDesc swapchainDesc{};
#endif
};

} // namespace fuse::editor
