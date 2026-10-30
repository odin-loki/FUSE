// fuse_app UIKit backend (iOS). UIKit owns the run loop, so the app hands its setup to uikitMain(): it runs
// UIApplicationMain, calls `setup` once the scene's UIWindow exists, then drives Application::step(0) from a
// CADisplayLink. The AppWindow is drawn by a FuseUIView (CPU raster -> CGImage layer contents) that takes touches
// (pointer down / move / up), the soft keyboard through UIKeyInput (insertText -> UiContext::textInput,
// deleteBackward -> Key::Backspace; shown while UiContext::wantsTextInput) and exposes UiContext::snapshotA11y as
// UIAccessibilityElements (traits from src/android_map.hpp fuse::app::uikit::traits) for VoiceOver.
//
// STATUS: UNVERIFIED. Written on Linux; uikit.mm has never been compiled or run (no iOS SDK on the build host).
// Only the platform-neutral tables (uikit::traits) are tested (fuse_app_mobile_map). Expect compile fixes on the
// first Xcode build.
#pragma once
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include <functional>

namespace fuse::app {

/// Runs the iOS app. `setup` gets the Application and the one full-screen AppWindow (already sized to the screen in
/// points) and loads its UI. Never returns while the app lives.
/// `o.vulkan` (default on): the view hosts a CAMetalLayer presented through Vulkan-on-Metal (MoltenVK,
/// VK_EXT_metal_surface + portability enumeration; UNVERIFIED), else CPU raster -> CGImage layer contents.
/// Text input is UITextInput with marked text (IME composition -> UiContext::textComposition; UNVERIFIED).
int uikitMain(int argc, char** argv, AppDesc desc, std::function<void(Application&, AppWindow&)> setup,
              const NativeOptions& o = {});

} // namespace fuse::app
