// fuse_app native windows (plan UI5): real OS windows (X11 via a runtime-loaded libX11, Win32, macOS Cocoa, iOS UIKit,
// Android NativeActivity) with a Vulkan swapchain presenter. The UI is drawn either by the fuse_ui_vk compute kernel
// straight into a storage image (GpuDraw) or by the CPU rasteriser and uploaded (CpuUpload); either result is blitted
// into the swapchain image and presented. Without a Vulkan device the CPU raster is shown by the OS: GDI on Win32
// (StretchDIBits), a CALayer's contents on macOS / iOS, ANativeWindow_lock on Android, XPutImage on X11 (24 / 32 bit
// TrueColor visuals; deeper or shallower ones keep the window but cannot draw it).
//
// Mobile and macOS notes:
//  - Android and iOS own the process entry point. Include fuse/app/main.hpp in the file that defines
//    `int main(int argc, char** argv)`: it becomes the app's main on every platform (android_main / UIApplicationMain
//    call it). Android apps are shared libraries loaded by NativeActivity (CMake: fuse_app_add_executable).
//  - One OS window per process on Android / iOS (the activity's surface / the screen): the first window shown gets
//    it, further windows stay headless.
//  - The UI works in logical units (dp / points); the frame is rasterised at the display's physical pixel size
//    (AppWindow::setScale / pixelSize, UiContext::setRenderScale), so text and edges are as sharp as the screen.
//    NativeOptions::scale picks the factor. Desktop: X11 reads Xft.dpi (see dpi::x11Scale), Win32 is per-monitor DPI
//    aware (GetDpiForWindow, WM_DPICHANGED); the OS window is logical size x scale pixels.
//  - Popups stay overlays inside the window (NativeOptions::osPopups is desktop X11 / Win32 only).
//  - macOS: Command acts as kModCtrl (Cmd+C / Cmd+V drive the widgets' Ctrl shortcuts), Control as kModSuper.
//
//   fuse::app::Application app{{.name = "hello"}};
//   fuse::app::NativePlatform native(app);          // installs the OS event source (blocks in poll / MsgWait)
//   auto& w = app.createWindow({.title = "Hello"});
//   native.show(w);                                 // OS window + presenter; headless when no display
//   return app.run();
//
// Declare NativePlatform after the Application (it is destroyed first and detaches its windows and event source).
#pragma once

#include "fuse/app/app.hpp"

#include <memory>
#include <span>
#include <vector>
#include <string>
#include <string_view>

namespace fuse::app {

enum class PresenterKind : fuse::ui::u8 {
    Headless, ///< no OS window (no display / not shown); CPU raster into AppWindow::image()
    Vulkan,   ///< swapchain presenter
    Software, ///< the OS shows the CPU raster (Win32 GDI, X11 XPutImage, macOS / iOS layer contents, Android window buffer)
};
enum class VkDrawMode : fuse::ui::u8 {
    Auto,      ///< GpuDraw when the compute kernel is compiled in, else CpuUpload
    GpuDraw,   ///< fuse_ui_vk "ui.draw_list" kernel renders the draw list on the GPU
    CpuUpload, ///< CPU raster of the damaged region, uploaded and blitted
};
/// Raw Vulkan handles (VkInstance / VkPhysicalDevice / VkDevice / VkQueue). As NativeOptions::adoptDevice the
/// presenter runs on this device instead of creating its own (the instance needs VK_KHR_surface + the platform surface
/// extension, the device VK_KHR_swapchain, the queue graphics or compute + present): GPU images of the same device
/// (an engine frame) can then be drawn by the UI kernel without a copy (setExternalTexture). The presenter submits on
/// `queue` from the UI thread; the owner keeps the device alive past the NativePlatform and must not use the queue
/// from another thread concurrently.
struct VulkanDeviceHandles {
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    unsigned queueFamily = 0;
};
/// A sampled image of the presenter's device for fuse_ui_vk's external slots (fuse::ui::Image::externalSlot).
struct ExternalTexture {
    void* image = nullptr;  ///< VkImage (null: unbind)
    void* view = nullptr;   ///< VkImageView, sampled usage
    unsigned layout = 0;    ///< its VkImageLayout when the UI frame is submitted; restored after the UI pass
};
struct NativeOptions {
    VulkanDeviceHandles adoptDevice{}; ///< instance + device set: present on that device (see VulkanDeviceHandles)
    bool vulkan = true;            ///< false: never create a Vulkan presenter
    VkDrawMode drawMode = VkDrawMode::Auto;
    bool vsync = true;             ///< FIFO (true) or MAILBOX / IMMEDIATE when available
    /// Popup menus / combo lists / submenus as their own borderless OS windows (Qt-style; may extend past the main
    /// window, clamp to the screen). X11: override-redirect windows; Win32: WS_POPUP + WS_EX_NOACTIVATE owned windows.
    bool osPopups = false;
    /// Logical-unit scale: UI units = display pixels / scale. 0 = the OS content scale (Android density / 160,
    /// UIScreen / NSScreen scale, Win32 monitor DPI / 96, X11 $FUSE_UI_SCALE, else Xft.dpi / 96, else $GDK_SCALE,
    /// else 1); 1 = work in physical pixels. The frame is always rendered at the display's pixel size (the window's
    /// AppWindow::scale() is set to this).
    float scale = 0.f;
};

#if defined(__ANDROID__)
/// The android_app (android_native_app_glue) this process runs in. The built-in android_main sets it before the
/// user's main; an app with its own android_main calls setAndroidApp first. Also points setDataRoot at the
/// activity's internal data path and installs APK-asset readers for resources and bundled fonts ("fonts/<name>").
void setAndroidApp(void* androidApp);
void* androidApp();
#endif

class NativePlatform {
public:
    explicit NativePlatform(Application& app, const NativeOptions& o = {});
    ~NativePlatform();
    NativePlatform(const NativePlatform&) = delete;
    NativePlatform& operator=(const NativePlatform&) = delete;

    /// True when an OS display connection is open (X11: $DISPLAY; Win32 / macOS: always; iOS: inside
    /// UIApplicationMain; Android: an android_app is set).
    bool available() const;
    /// Opens an OS window for `w` (title / size from its WindowDesc) and installs the best presenter.
    PresenterKind show(AppWindow& w);
    PresenterKind kind(const AppWindow& w) const;
    /// "lavapipe (LLVM ...)" etc. once a Vulkan presenter exists; empty otherwise.
    std::string_view gpuName() const;
    /// True when the GPU kernel (not the CPU upload path) draws the window.
    bool gpuDraws(const AppWindow& w) const;
    std::string_view lastError() const;
    /// Reads back what the OS shows in the window's client area (tests / screenshots of real windows).
    bool capture(const AppWindow& w, fuse::ui::Image& out);
    /// Native handles: X11 Display* / Window, Win32 HINSTANCE / HWND, macOS NSApplication* / NSWindow*, iOS
    /// UIApplication* / UIWindow*, Android android_app* / ANativeWindow* (0 when not shown).
    void* nativeDisplay() const;
    /// Binds `tex` to external slot `slot` (0..3) of the window's GPU draw kernel; images whose externalSlot == slot
    /// then show it. Re-set it whenever the image, view or its layout change (each engine frame). False when the
    /// window is not GPU drawn (the CPU pixels of the Image are shown instead).
    bool setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex);
    unsigned long long nativeWindow(const AppWindow& w) const;
    /// Sets the OS window icon from square RGBA rasters (e.g. 16 / 32 / 48 / 256). X11: _NET_WM_ICON with every size;
    /// Win32: WM_SETICON big (SM_CXICON) and small (SM_CXSMICON), the closest raster each. False when not shown.
    bool setIcon(const AppWindow& w, std::span<const fuse::ui::Image> sizes);
    /// Asks the OS to resize the window's client area (logical px); the resize then arrives like a user resize
    /// (ConfigureNotify / WM_SIZE -> AppWindow::resize). False when not shown.
    bool resizeWindow(const AppWindow& w, unsigned width, unsigned height);
    /// Read back from the OS: the icon sizes (widths) the window carries now (X11 every _NET_WM_ICON entry; Win32 the
    /// big + small WM_GETICON icons). Empty when unset / not shown.
    std::vector<int> iconSizes(const AppWindow& w) const;

    /// OS-window popups (NativeOptions::osPopups): how many popup windows `w` shows now, each one's native handle,
    /// its screen rect (physical px) and a read-back of its pixels.
    std::size_t popupCount(const AppWindow& w) const;
    unsigned long long popupWindow(const AppWindow& w, std::size_t i) const;
    fuse::ui::Rect popupScreenRect(const AppWindow& w, std::size_t i) const;
    bool capturePopup(const AppWindow& w, std::size_t i, fuse::ui::Image& out);
    /// The window's top-left on screen (client area).
    fuse::ui::Vec2 screenOrigin(const AppWindow& w) const;
    /// The part of the window not covered by system UI (notch, status / navigation bars, home indicator), in UI
    /// units. The whole window on desktop and when not shown.
    fuse::ui::Rect safeArea(const AppWindow& w) const;
    /// UI units -> display pixels for this window (see NativeOptions::scale). Equals the shown window's
    /// AppWindow::scale().
    float scale(const AppWindow& w) const;
    /// Posts a real left click to a native window (XSendEvent / PostMessage) at window-local `at` (window pixels); it
    /// is then delivered by the normal event pump (tests drive popup windows this way).
    bool postClick(unsigned long long nativeWin, fuse::ui::Vec2 at);

    // ---- platform services (plan §4.13). Window management itself is on AppWindow (setTitle, setMode, setVisible,
    // setPosition, activate, size limits, events, close guard, setMenu): show() connects it to the OS window.
    //   X11: title, size limits (WM_NORMAL_HINTS), show / hide, position, focus events always; maximise / fullscreen /
    //        minimise / activate are EWMH requests and need a window manager (the state then stays as it is).
    //   Win32, macOS: all of it. iOS / Android: one fullscreen surface, every request reports "cannot".
    /// The cursor shape last applied to the OS window: after every event pump the window's UiContext::cursor() is
    /// shown (X11: themed cursors through libXcursor when present, else the core cursor font; Win32: system cursors;
    /// macOS: NSCursor). Arrow on iOS / Android (no pointer cursor API in the NDK) and when not shown.
    fuse::ui::Cursor cursor(const AppWindow& w) const;
    /// The system clipboard (UTF-8 text); show() installs it as the window's ClipboardHooks. X11: the CLIPBOARD
    /// selection as UTF8_STRING (no INCR transfers; the content is lost when the app exits without a clipboard
    /// manager); Win32: CF_UNICODETEXT. False when unavailable / empty.
    bool setClipboardText(std::string_view utf8);
    bool clipboardText(std::string& out);
    /// Asks the OS for the light / dark preference and accent colour and stores it in Application::systemTheme()
    /// (windows get WindowEvent::ThemeChanged; AppWindow::setFollowSystemTheme restyles). Win32: registry
    /// (AppsUseLightTheme, DWM AccentColor) + change notification; macOS: effectiveAppearance / controlAccentColor +
    /// notification; iOS: UITraitCollection; Android: Configuration night mode (+ on configuration change);
    /// X11: $GTK_THEME, else `gsettings` once, else Unknown - and no change notification.
    SystemTheme systemTheme();
    /// Connected displays (bounds / work area in physical pixels, scale, refresh rate where the platform tells).
    std::vector<DisplayInfo> displays() const;
    /// The platform's own file dialog, modal (blocks until answered). Win32: GetOpen/SaveFileNameW, SHBrowseForFolder;
    /// macOS: NSOpenPanel / NSSavePanel; Linux / BSD: `zenity`, else `kdialog`, when installed; iOS / Android and
    /// desktops without a helper: Unavailable.
    FileDialogStatus fileDialog(const AppWindow* parent, const FileDialogOptions& o, std::string& path);
    /// Native dialog when there is one, else the in-toolkit widgets::fileDialog in `w`. `cb` gets the path ("" =
    /// cancelled): before this returns with a native dialog, later (from the UI) with the toolkit one.
    void openFileDialog(AppWindow& w, const FileDialogOptions& o, FileDialogCallback cb);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

/// Opens `url` with the user's default application (xdg-open / ShellExecuteW / NSWorkspace / UIApplication /
/// an ACTION_VIEW Intent). False when the platform has no way (no xdg-open in PATH).
bool openUrl(std::string_view url);
/// Shows `path` in the file manager (Explorer / Finder select it; Linux / BSD open its folder). False on iOS / Android.
bool revealFile(std::string_view path);

} // namespace fuse::app
