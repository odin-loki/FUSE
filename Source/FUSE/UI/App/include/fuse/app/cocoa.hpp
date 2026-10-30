// fuse_app Cocoa backend (G3, macOS). NSWindow + a layer-backed NSView presenting the CPU raster through a
// CGImage (CAMetalLayer / MoltenVK later), NSEvent mouse / key / scroll input, NSTextInputClient IME (marked text
// -> UiContext::textComposition, candidate window at UiContext::textCaret), and NSAccessibility elements built from
// UiContext::snapshotA11y.
//
// STATUS: UNVERIFIED. Written on Linux; cocoa.mm has never been compiled or run (no Apple SDK on the build host).
// Only the platform-neutral tables (src/cocoa_map.hpp) are tested (fuse_app_cocoa_map). Expect compile fixes on
// first macOS build.
#pragma once
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include <memory>

namespace fuse::app {

class CocoaPlatform {
public:
    /// `o.vulkan` (default on): each view is backed by a CAMetalLayer presented through Vulkan-on-Metal (MoltenVK /
    /// KosmicKrisp, VK_EXT_metal_surface + portability enumeration; UNVERIFIED). Falls back to CGImage layer contents
    /// when no Vulkan loader/ICD is present or the build has no Vulkan.
    explicit CocoaPlatform(Application& app, const NativeOptions& o = {});
    ~CocoaPlatform();
    CocoaPlatform(const CocoaPlatform&) = delete;
    CocoaPlatform& operator=(const CocoaPlatform&) = delete;
    bool available() const;
    bool show(AppWindow& w);
    /// NSWindow* for `w` (nullptr when not shown).
    void* nativeWindow(const AppWindow& w) const;
    /// True when `w` presents through the Vulkan swapchain (false: CGImage fallback or not shown yet).
    bool gpuPresents(const AppWindow& w) const;
    const char* gpuName() const;
    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace fuse::app
