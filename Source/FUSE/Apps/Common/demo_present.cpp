#include "demo_present.hpp"

#include <fuse/core/track_b.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/hybrid/vulkan_presentable.hpp>
#include <fuse/platform/window_wsi.hpp>
#include <fuse/renderer/vk/present_path.hpp>

#include <cstdlib>
#include <cstring>

namespace fuse::demo::present {

bool requestedFromArgv(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--present") == 0) {
            return true;
        }
    }
    const char* env = std::getenv("FUSE_DEMO_PRESENT");
    return env != nullptr && (std::strcmp(env, "game") == 0 || std::strcmp(env, "1") == 0);
}

bool gameWindowPresentAvailable() {
#if defined(FUSE_PLATFORM_WINDOW_WIN32) || defined(FUSE_PLATFORM_WINDOW_X11)
    return fuse::platform::windowWsiAvailable() && fuse::platform::displayServerAvailable();
#else
    return false;
#endif
}

void applyGameWindowToBootstrapDesc(fuse::hybrid::HybridRendererBootstrapDesc& desc, u32 width, u32 height,
                                    const char* windowTitle) {
    desc.presentable.backend = fuse::hybrid::PresentableBackend::GameWindow;
    desc.presentable.window.title = windowTitle != nullptr ? windowTitle : "fuse_demo_present";
    desc.presentable.window.width = width;
    desc.presentable.window.height = height;
    desc.presentable.swapchainWidth = width;
    desc.presentable.swapchainHeight = height;
    desc.scene.width = 0;
    desc.scene.height = 0;
}

HostPresentUnlock::HostPresentUnlock() {
    if (gameWindowPresentAvailable()) {
        fuse::core::setTrackBHostFeature(fuse::core::TrackBHostFeature::EditorViewportPresent, true);
        m_active = true;
    }
}

HostPresentUnlock::~HostPresentUnlock() {
    if (m_active) {
        fuse::core::setTrackBHostFeature(fuse::core::TrackBHostFeature::EditorViewportPresent, false);
        m_active = false;
    }
}

u32 realPresentCallCount(const fuse::hybrid::HybridRendererBootstrap& runtime) {
    if (runtime.presentPath() == nullptr) {
        return 0u;
    }
    return runtime.presentPath()->status().realPresentCallCount;
}

std::string projectPathFromArgv(int argc, char** argv, const std::string& defaultPath) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--present") == 0) {
            continue;
        }
        return argv[i];
    }
    return defaultPath;
}

} // namespace fuse::demo::present
