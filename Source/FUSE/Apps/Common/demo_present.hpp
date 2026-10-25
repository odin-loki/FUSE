#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::hybrid {
struct HybridRendererBootstrapDesc;
class HybridRendererBootstrap;
} // namespace fuse::hybrid

namespace fuse::demo::present {

/// True when `argv` contains `--present` or env `FUSE_DEMO_PRESENT` is `game` / `1`.
bool requestedFromArgv(int argc, char** argv);

/// X11 display + GameWindow backend available (Xvfb on CI).
[[nodiscard]] bool gameWindowPresentAvailable();

/// Configure bootstrap for `PresentableBackend::GameWindow` (follow swapchain size).
void applyGameWindowToBootstrapDesc(fuse::hybrid::HybridRendererBootstrapDesc& desc, u32 width, u32 height,
                                    const char* windowTitle);

/// RAII: enable `TrackBHostFeature::EditorViewportPresent` for U8 / Xvfb game-window present (see
/// `test_hybrid_frame_vk` swapchain mode).
class HostPresentUnlock {
public:
    HostPresentUnlock();
    ~HostPresentUnlock();

    HostPresentUnlock(const HostPresentUnlock&) = delete;
    HostPresentUnlock& operator=(const HostPresentUnlock&) = delete;

private:
    bool m_active = false;
};

[[nodiscard]] fuse::u32 realPresentCallCount(const fuse::hybrid::HybridRendererBootstrap& runtime);

/// Parse project path from argv, skipping `--present`.
std::string projectPathFromArgv(int argc, char** argv, const std::string& defaultPath);

} // namespace fuse::demo::present
