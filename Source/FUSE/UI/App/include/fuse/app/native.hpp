// fuse_app native windows (plan UI5): real OS windows (X11 via a runtime-loaded libX11, Win32, macOS Cocoa, iOS UIKit,
// Android NativeActivity) with a Vulkan swapchain presenter. The UI is drawn either by the fuse_ui_vk compute kernel
// straight into a storage image (GpuDraw) or by the CPU rasteriser and uploaded (CpuUpload); either result is blitted
// into the swapchain image and presented. Without a Vulkan device the CPU raster is shown by the OS: GDI on Win32
// (StretchDIBits), a CALayer's contents on macOS / iOS, ANativeWindow_lock on Android, XPutImage on X11 (24 / 32 bit
// TrueColor visuals; deeper or shallower ones keep the window but cannot draw it), wl_shm buffers on Wayland.
// Linux / BSD have two window systems, X11 and Wayland (a runtime-loaded libwayland-client), picked when the
// NativePlatform is made: see NativeBackend. What Wayland cannot do by protocol (window position, activation, a
// minimised state) is reported as "cannot"; docs/ui/README.md has the list.
//
// Mobile and macOS notes:
//  - Android and iOS own the process entry point. Include fuse/app/main.hpp and define the entry as
//    `int FUSE_APP_MAIN(int argc, char** argv)`: it is main on desktop and the function android_main /
//    UIApplicationMain call on Android / iOS. Android apps are shared libraries loaded by NativeActivity (CMake: fuse_app_add_executable).
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

#include <expected>
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
/// The window system of a Linux / BSD desktop (the other platforms have one and ignore it).
enum class NativeBackend : fuse::ui::u8 {
    Auto,    ///< $FUSE_UI_BACKEND (x11 | wayland) if set; else Wayland when $WAYLAND_DISPLAY is set and libwayland-client
             ///< loads and a compositor answers; else X11 ($DISPLAY)
    X11,     ///< libX11 (also under XWayland)
    Wayland, ///< libwayland-client: xdg-shell toplevels, wl_shm / VK_KHR_wayland_surface (see docs/ui/README.md)
};
struct NativeOptions {
    VulkanDeviceHandles adoptDevice{}; ///< instance + device set: present on that device (see VulkanDeviceHandles)
    bool vulkan = true;            ///< false: never create a Vulkan presenter
    VkDrawMode drawMode = VkDrawMode::Auto;
    bool vsync = true;             ///< FIFO (true) or MAILBOX / IMMEDIATE when available
    /// Popup menus / combo lists / submenus as their own borderless OS windows (Qt-style; may extend past the main
    /// window, clamp to the screen). X11: override-redirect windows; Win32: WS_POPUP + WS_EX_NOACTIVATE owned windows;
    /// Wayland: xdg_popups placed relative to the window (the compositor keeps them on screen).
    bool osPopups = false;
    /// Logical-unit scale: UI units = display pixels / scale. 0 = the OS content scale (Android density / 160,
    /// UIScreen / NSScreen scale, Win32 monitor DPI / 96, X11 $FUSE_UI_SCALE, else XSETTINGS Xft/DPI (followed
    /// live: every window is re-scaled when it changes), else Xft.dpi / 96, else $GDK_SCALE, else 1; Wayland
    /// $FUSE_UI_SCALE, else the compositor's preferred fractional scale, else the wl_output scale); 1 = work in
    /// physical pixels. The frame is always rendered at the display's pixel size (the window's
    /// AppWindow::scale() is set to this).
    float scale = 0.f;
    /// OS tool windows (UiContext::setOsWindows): overlay nodes with the class "os-window" - floating dock panels
    /// (widgets::dockFloat) - each get their own borderless OS window owned by the main one, placed at window origin
    /// + node rect, so a floated panel can leave the main window. Pointer input comes back in context coordinates,
    /// keys keep going to the main window. X11: override-redirect utility windows; Win32: WS_EX_TOOLWINDOW owned
    /// popups. Elsewhere (Wayland included), and when off (the default), floating docks stay in-window panels.
    bool osWindows = false;
    /// OS drag and drop (X11: XDND v5; Win32: OLE; Wayland: drops in only). In: files / text dragged from another
    /// application reach the window's drop targets through UiContext::dragExternalMove / dropExternal (DragData type
    /// kDragFiles with LF-separated paths in `text`, or kDragText). Out (X11 / Win32): a drag started in the UI
    /// (UiContext::beginDrag) whose data is kDragText / kDragFiles continues over other applications once the pointer
    /// leaves the app's windows; the source gets DragEnd with what the other side did
    /// (UiContext::dragFinishExternal).
    bool dragAndDrop = true;
    /// Linux / BSD: which window system to use. A forced backend (here or by $FUSE_UI_BACKEND) that cannot start
    /// leaves the platform unavailable (headless) instead of falling back to the other one.
    NativeBackend backend = NativeBackend::Auto;
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
    /// Linux / BSD: the window system in use (X11 or Wayland); Auto when none is open and on the other platforms.
    NativeBackend backend() const;
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
    /// Native handles: X11 Display* / Window, Wayland wl_display* / wl_surface*, Win32 HINSTANCE / HWND, macOS
    /// NSApplication* / NSWindow*, iOS UIApplication* / UIWindow*, Android android_app* / ANativeWindow* (0 when not
    /// shown).
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
    /// is then delivered by the normal event pump (tests drive popup windows this way). Wayland has no way to post
    /// input to a surface: the click is delivered inside the process, below the wl_pointer handlers.
    bool postClick(unsigned long long nativeWin, fuse::ui::Vec2 at);
    /// Posts a pointer move / a left button press or release / a close request (the title-bar close button) to a
    /// native window at window-local device pixels `at`; delivered by the normal event pump. `at` may lie outside
    /// the window (a drag that left it).
    bool postPointerMove(unsigned long long nativeWin, fuse::ui::Vec2 at);
    bool postPointerButton(unsigned long long nativeWin, fuse::ui::Vec2 at, bool down);
    bool postClose(unsigned long long nativeWin);

    /// OS tool windows (NativeOptions::osWindows): how many `w` shows now (back to front), each one's native handle,
    /// its screen rect (physical px) and a read-back of its pixels.
    std::size_t toolWindowCount(const AppWindow& w) const;
    unsigned long long toolWindow(const AppWindow& w, std::size_t i) const;
    fuse::ui::Rect toolWindowScreenRect(const AppWindow& w, std::size_t i) const;
    bool captureToolWindow(const AppWindow& w, std::size_t i, fuse::ui::Image& out);
    /// True while a drag that started in this app is over, or being dropped on, another application
    /// (NativeOptions::dragAndDrop).
    bool externalDragActive() const;

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
    /// Clipboard formats (services.hpp ClipboardItem): replaces the clipboard with `items`, one per format.
    ///   X11: CLIPBOARD on the platform's connection: TARGETS, UTF8_STRING / text/plain(;charset=utf-8), image/png,
    ///        custom MIME types as atoms of that name, INCR both ways for large data; lazy items are made when first
    ///        pasted. Other clients can paste while this platform pumps events.
    ///   Win32: CF_UNICODETEXT, CF_DIBV5, RegisterClipboardFormat(mime) for custom types (lazy items are made at once).
    ///   macOS / iOS / Android: text only (the other formats are refused).
    bool setClipboard(std::vector<ClipboardItem> items);
    bool setClipboardImage(const fuse::ui::Image& image);
    bool clipboardImage(fuse::ui::Image& out);
    /// Raw bytes of any format (text: UTF-8; images: the PNG file).
    bool clipboardData(std::string_view mime, std::vector<fuse::ui::u8>& out);
    /// MIME types offered now (kClipboardText / kClipboardImage for the OS text and image formats).
    std::vector<std::string> clipboardFormats();
    /// True while this platform owns the clipboard (another application has not replaced it since).
    bool clipboardOwned() const;
    /// The X11 PRIMARY selection (select to copy, middle click to paste), text only. False elsewhere.
    bool setPrimarySelectionText(std::string_view utf8);
    bool primarySelectionText(std::string& out);
    /// Asks the OS for the light / dark preference and accent colour and stores it in Application::systemTheme()
    /// (windows get WindowEvent::ThemeChanged; AppWindow::setFollowSystemTheme restyles). Win32: registry
    /// (AppsUseLightTheme, DWM AccentColor) + change notification; macOS: effectiveAppearance / controlAccentColor +
    /// notification; iOS: UITraitCollection; Android: Configuration night mode (+ on configuration change);
    /// X11: XSETTINGS (Net/ThemeName, Gtk/ApplicationPreferDarkTheme; followed live while the settings manager that
    /// ran at start keeps running), else $GTK_THEME, else `gsettings` once, else Unknown.
    SystemTheme systemTheme();
    /// X11: the size (px) themed cursors are loaded at: the XSETTINGS Gtk/CursorThemeSize when a settings manager
    /// runs, else libXcursor's default (Xcursor.size / $XCURSOR_SIZE). 0 without libXcursor and on other platforms.
    int cursorThemeSize() const;
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

// ---- window-less native dialogs (types in services.hpp). Modal: they block the calling (UI) thread until answered.
//   Win32: IFileOpenDialog / IFileSaveDialog (FOS_PICKFOLDERS for folders), ChooseColorW, MessageBoxW.
//   Linux / BSD: zenity, else kdialog (messageBox: zenity only); Unavailable when no helper is installed or there is
//   no display.
//   macOS / iOS / Android: Unavailable (use NativePlatform::fileDialog / the in-toolkit dialogs).
/// Paths are UTF-8.
std::expected<std::vector<std::string>, PlatformError> openFileDialog(const FileDialogDesc& d);
std::expected<std::string, PlatformError> saveFileDialog(const FileDialogDesc& d);
std::expected<std::string, PlatformError> pickFolderDialog(const FileDialogDesc& d);
/// Colour picker. Alpha is passed through unchanged.
std::expected<fuse::ui::Color, PlatformError> colorDialog(const ColorDialogDesc& d);
std::expected<MessageResult, PlatformError> messageBox(std::string_view title, std::string_view text, MessageIcon icon = MessageIcon::Info,
                                                       MessageButtons buttons = MessageButtons::Ok, unsigned long long owner = 0);
/// revealFile with an error: Failed when `path` does not exist, Unavailable when the platform has no file manager hook.
std::expected<void, PlatformError> revealInFileManager(std::string_view path);

} // namespace fuse::app
