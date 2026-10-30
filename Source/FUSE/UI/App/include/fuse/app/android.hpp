// fuse_app Android backend (G4). NDK NativeActivity via android_native_app_glue: the ANativeWindow is the one
// AppWindow (CPU raster blitted with ANativeWindow_lock), touch + mouse + hardware keys from AInputQueue, and - when
// the activity is org.fuse.ui.FuseActivity (android/java/org/fuse/ui/FuseActivity.java) - a real IME through an
// InputConnection (commit + composing text -> UiContext::textInput / textComposition) and TalkBack through an
// AccessibilityNodeProvider fed from UiContext::snapshotA11y. With a plain android.app.NativeActivity, text falls
// back to KeyEvent.getUnicodeChar and there is no accessibility tree.
//
// Presentation: with NativeOptions::vulkan (default) and a device that can present to the ANativeWindow
// (VK_KHR_android_surface; every Android 7+ device with Vulkan, the emulator with -gpu swiftshader_indirect / host),
// the window presents through the shared Vulkan swapchain presenter (GPU draw kernel or CPU-raster upload, see
// NativeOptions::drawMode). Otherwise the CPU raster is blitted with ANativeWindow_lock. The swapchain is dropped on
// APP_CMD_TERM_WINDOW and rebuilt on the next APP_CMD_INIT_WINDOW.
//
//   void android_main(android_app* state) {
//       fuse::app::androidInstallFonts(state);
//       fuse::app::Application app{{.name = "hello", .loadSettings = false}};
//       fuse::app::AndroidPlatform platform(app, state);
//       auto& w = app.createWindow({.title = "Hello"});
//       w.loadText(...);
//       platform.show(w);
//       app.run();
//   }
#pragma once
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include <memory>
#include <string_view>

struct android_app;

namespace fuse::app {

/// Copies the APK's assets/fonts tree to the app's internal data dir and points FUSE_TEXT_FONT_DIR at it. Call first
/// in android_main, before the Application (which loads the bundled font) is constructed. False if nothing was found.
bool androidInstallFonts(android_app* state);

class AndroidPlatform {
public:
    AndroidPlatform(Application& app, android_app* state, const NativeOptions& o = {});
    ~AndroidPlatform();
    AndroidPlatform(const AndroidPlatform&) = delete;
    AndroidPlatform& operator=(const AndroidPlatform&) = delete;
    /// Binds `w` to the activity's ANativeWindow (resized to it whenever the surface changes).
    bool show(AppWindow& w);
    /// Has a surface (between APP_CMD_INIT_WINDOW and APP_CMD_TERM_WINDOW).
    bool hasSurface() const;
    /// True while the window presents through Vulkan (false: ANativeWindow_lock CPU blit).
    bool gpuPresents() const;
    /// The Vulkan device name once a swapchain exists; empty otherwise.
    std::string_view gpuName() const;
    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace fuse::app
