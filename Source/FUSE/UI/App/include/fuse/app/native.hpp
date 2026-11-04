// fuse_app native windows (plan UI5): real OS windows (X11 via a runtime-loaded libX11, Win32) with a Vulkan swapchain
// presenter. The UI is drawn either by the fuse_ui_vk compute kernel straight into a storage image (GpuDraw) or by the
// CPU rasteriser and uploaded (CpuUpload); either result is blitted into the swapchain image and presented. Without a
// Vulkan device the CPU raster is blitted by the OS (Win32 GDI, X11 XPutImage). By default (asyncVulkan) the first
// window starts on that software path and switches to Vulkan once a worker thread has created instance, device and
// swapchain, so the first frame does not wait for the driver.
//
//   fuse::app::Application app{{.name = "hello"}};
//   fuse::app::NativePlatform native(app);          // installs the OS event source (blocks in poll / MsgWait)
//   auto& w = app.createWindow({.title = "Hello"});
//   native.show(w);                                 // OS window + presenter; headless when no display
//   return app.run();
//
// Declare NativePlatform after the Application (it is destroyed first and detaches its windows and event source).
//
// Android (NativeActivity through android_native_app_glue, fuse_app_add_android_app in CMake): the app keeps its
// plain main(); the glue runs it on the activity's app thread with the APK assets extracted to the working directory.
// show() waits for the activity's window and returns Vulkan (ANativeWindow -> VkSurfaceKHR, same swapchain presenter)
// or Software (ANativeWindow_lock of the CPU raster). The window is the whole activity; the UI scale follows the
// screen density. Lifecycle: the swapchain and surface are released when the window goes away (home, lock screen)
// and rebuilt when it returns, the UI state stays; fuse::platform lifecycle / surface-loss callbacks fire when
// fuse_core is linked. Touch drives the pointer (a vertical drag over a scroll container pans it); popups are
// in-window overlays; the soft keyboard follows UiContext::wantsTextInput and delivers committed text.
#pragma once

#include "fuse/app/app.hpp"
#include "fuse/app/platform.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <string>
#include <string_view>

namespace fuse::app {

enum class PresenterKind : fuse::ui::u8 {
    Headless, ///< no OS window (no display / not shown); CPU raster into AppWindow::image()
    Vulkan,   ///< swapchain presenter
    Software, ///< CPU raster blitted by the OS (Win32 GDI, X11 XPutImage): no Vulkan, or while Vulkan starts (asyncVulkan)
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
    /// DPI: Win32 per-monitor-v2 awareness (each window takes its monitor's scale and follows WM_DPICHANGED);
    /// X11 XSETTINGS Xft/DPI (followed live), else RESOURCE_MANAGER Xft.dpi, overridden by $FUSE_UI_SCALE. False:
    /// every window stays at scale 1.
    bool dpiAware = true;
    /// Win32 WM_POINTER touch / pen: the primary finger and the pen drive the pointer, every contact reaches
    /// UiContext::touches() (false: Windows synthesises mouse messages instead).
    bool touch = true;
    /// Replaces the OS theme query (kiosk apps, tests); null = the OS settings. Re-read on WM_SETTINGCHANGE /
    /// WM_THEMECHANGED / WM_DWMCOLORIZATIONCOLORCHANGED.
    SystemTheme (*themeQuery)(void* user) = nullptr;
    void* themeQueryUser = nullptr;
    /// Fast first frame: the first window shows the CPU raster through the software presenter at once while the
    /// Vulkan instance / device / swapchain are created on a worker thread; the next frame after that finishes goes
    /// through Vulkan (the whole current UI frame, so nothing flickers). false: show() blocks until Vulkan is ready.
    /// Ignored with adoptDevice and once the platform has a Vulkan device (later windows start on Vulkan directly).
    bool asyncVulkan = true;
    /// Device pixels per UI unit for shown windows; 0 = the OS value (Android: screen density / 160, desktop: 1).
    fuse::ui::f32 scale = 0.f;
    /// Show / hide the soft keyboard as text fields gain / lose focus (mobile).
    bool autoSoftKeyboard = true;
};

/// Insets of the area the OS draws over (status / navigation bars, display cutouts), in UI units. Zero on desktop.
struct Insets {
    fuse::ui::f32 left = 0.f, top = 0.f, right = 0.f, bottom = 0.f;
};
/// Optional callbacks (function table, like Presenter / EventSource). Called on the UI thread from the event pump.
struct NativeHooks {
    void* user = nullptr;
    void (*insetsChanged)(void* user, AppWindow& w, const Insets& in) = nullptr;
    void (*visibility)(void* user, bool foreground) = nullptr;
    void (*lowMemory)(void* user) = nullptr;
};
/// Mobile lifecycle counters (tests, diagnostics).
struct LifecycleStats {
    unsigned surfacesCreated = 0; ///< OS windows / surfaces bound to a presenter
    unsigned surfacesLost = 0;    ///< released (window terminated)
    unsigned pauses = 0, resumes = 0, configChanges = 0, lowMemory = 0;
    bool foreground = true;
    bool hasSurface = false;
    bool softKeyboard = false;    ///< the soft keyboard was last requested shown
};

/// Per-window OS message hook (Win32: runs first in the window procedure; return true to consume the message with
/// `result` as its LRESULT). Accessibility adapters answer WM_GETOBJECT through it. Not called on X11.
struct MessageHook {
    void* user = nullptr;
    bool (*fn)(void* user, unsigned msg, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result) = nullptr;
};

class NativePlatform {
public:
    explicit NativePlatform(Application& app, const NativeOptions& o = {});
    ~NativePlatform();
    NativePlatform(const NativePlatform&) = delete;
    NativePlatform& operator=(const NativePlatform&) = delete;

    /// True when an OS display connection is open (X11: $DISPLAY; Win32: always).
    bool available() const;
    /// Opens an OS window for `w` (title / size from its WindowDesc) and installs the best presenter.
    PresenterKind show(AppWindow& w);
    PresenterKind kind(const AppWindow& w) const;
    /// True while the window shows the software presenter and its Vulkan presenter is still being created
    /// (NativeOptions::asyncVulkan); the event pump switches it over as soon as it is ready.
    bool presenterPending(const AppWindow& w) const;
    /// Waits (pumping OS messages) until a pending Vulkan presenter is ready and switches to it; returns the kind.
    PresenterKind waitForPresenter(const AppWindow& w);
    /// "lavapipe (LLVM ...)" etc. once a Vulkan presenter exists; empty otherwise.
    std::string_view gpuName() const;
    /// True when the GPU kernel (not the CPU upload path) draws the window.
    bool gpuDraws(const AppWindow& w) const;
    std::string_view lastError() const;
    /// Reads back what the OS shows in the window's client area (tests / screenshots of real windows).
    bool capture(const AppWindow& w, fuse::ui::Image& out);
    /// Native handles: X11 Display* / Window, Win32 HINSTANCE / HWND (0 when not shown).
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

    /// Mobile: safe-area insets of the window (UI units); zero on desktop.
    Insets safeAreaInsets(const AppWindow& w) const;
    /// Mobile: shows / hides the soft keyboard (NativeOptions::autoSoftKeyboard does it from focus). No-op on desktop.
    void setSoftKeyboard(bool show);
    void setHooks(const NativeHooks& h);
    LifecycleStats lifecycle() const;
    /// Android: the android_app* of the running activity (JNI, assets); null elsewhere.
    static void* androidApp();

    /// OS-window popups (NativeOptions::osPopups): how many popup windows `w` shows now, each one's native handle,
    /// its screen rect (physical px) and a read-back of its pixels.
    std::size_t popupCount(const AppWindow& w) const;
    unsigned long long popupWindow(const AppWindow& w, std::size_t i) const;
    fuse::ui::Rect popupScreenRect(const AppWindow& w, std::size_t i) const;
    bool capturePopup(const AppWindow& w, std::size_t i, fuse::ui::Image& out);
    /// The window's top-left on screen (client area).
    fuse::ui::Vec2 screenOrigin(const AppWindow& w) const;
    /// Posts a real left click to a native window (XSendEvent / PostMessage) at window-local `at`; it is then
    /// delivered by the normal event pump (tests drive popup windows this way).
    bool postClick(unsigned long long nativeWin, fuse::ui::Vec2 at);
    /// Installs (or with an empty hook removes) the message hook of a shown window. False when not shown.
    bool setMessageHook(const AppWindow& w, const MessageHook& hook);

    // ---- platform services (plan §4.13) ----
    /// Monitors (Win32 EnumDisplayMonitors + GetDpiForMonitor + EnumDisplaySettings; X11 RandR 1.5 monitors through a
    /// runtime-loaded libXrandr, work area from _NET_WORKAREA, else the default screen).
    std::vector<DisplayInfo> displays() const;
    /// The OS theme now (or NativeOptions::themeQuery). The Application is updated at start and on every change.
    SystemTheme querySystemTheme() const;
    /// Changes a window's DPI scale the way a monitor change does (the window keeps its logical size and is resized
    /// to logical * scale device pixels). X11 has one DPI per screen (XSETTINGS Xft/DPI changes re-scale every window);
    /// per-window changes go through this.
    bool setWindowScale(const AppWindow& w, float scale);
    /// The OS cursor shape last applied to the window (Win32 WM_SETCURSOR / SetCursor, X11 XDefineCursor).
    fuse::ui::CursorShape appliedCursor(const AppWindow& w) const;
    /// Win32: whether the title bar was last switched to dark (DWMWA_USE_IMMERSIVE_DARK_MODE accepted).
    bool darkTitleBar(const AppWindow& w) const;
    /// Re-reads the OS theme and applies it (what WM_SETTINGCHANGE on Win32 and an XSETTINGS Net/ThemeName change on
    /// X11 do by themselves).
    void refreshSystemTheme();
    /// Whether the window has keyboard focus (Win32 WM_SETFOCUS / WM_KILLFOCUS, X11 FocusIn / FocusOut).
    bool windowFocused(const AppWindow& w) const;
    /// Thread-safe: wakes the event pump if it is waiting (Win32: a posted WM_NULL; X11: a self-pipe the pump polls).
    /// Accessibility adapters call it after queueing an action from an AT thread.
    void wake();
    /// Posts a pointer move (WM_MOUSEMOVE / MotionNotify) to a native window at window-local device pixels `at`.
    bool postPointerMove(unsigned long long nativeWin, fuse::ui::Vec2 at);
    /// Asks a native window to close the way its close button does (WM_CLOSE / WM_DELETE_WINDOW).
    bool postClose(unsigned long long nativeWin);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace fuse::app
