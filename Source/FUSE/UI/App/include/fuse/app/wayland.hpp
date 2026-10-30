// fuse_app Wayland backend (G2): native libwayland-client, loaded at run time through the OS seam (no build-time
// wayland dependency, like the X11 backend). xdg-shell toplevels, wl_shm software presentation of the CPU raster,
// wl_seat pointer + keyboard (XKB keymaps via a runtime-loaded libxkbcommon, client-side key repeat from
// wl_keyboard.repeat_info), IME through text-input-unstable-v3 (preedit -> UiContext::textComposition, commit ->
// textInput, the caret reported with set_cursor_rectangle), xdg_popup menus / popups (showPopup), and system drag and drop both ways
// over wl_data_device (AppWindow::onExternalDrop / beginExternalDrag).
//
//   fuse::app::Application app{{.name = "hello"}};
//   fuse::app::WaylandPlatform wl(app);            // connects to $WAYLAND_DISPLAY
//   if (wl.available()) wl.show(app.createWindow({.title = "Hello"}));
//   else { /* fall back to NativePlatform (X11 / XWayland) */ }
//
// Declare WaylandPlatform after the Application.
//
// Presentation: with NativeOptions::vulkan (the default) and a Vulkan loader + ICD exposing VK_KHR_wayland_surface,
// each window presents through a Vulkan swapchain on its wl_surface (the same presenter as X11 / Win32: GPU draw
// kernel or CPU-raster upload, see NativeOptions::drawMode). Without one it falls back to wl_shm buffers of the CPU
// raster. gpuPresents() tells which one a window got.
#pragma once
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include <memory>
#include <string_view>

namespace fuse::app {

class WaylandPlatform {
public:
    explicit WaylandPlatform(Application& app, const char* displayName = nullptr, const NativeOptions& o = {});
    ~WaylandPlatform();
    WaylandPlatform(const WaylandPlatform&) = delete;
    WaylandPlatform& operator=(const WaylandPlatform&) = delete;

    /// Connected, with wl_compositor + wl_shm + xdg_wm_base.
    bool available() const;
    std::string_view lastError() const;
    /// xdg_toplevel for `w`, presented through wl_shm. False when unavailable.
    bool show(AppWindow& w);
    /// xdg_popup for `w` (a menu, completion list, tooltip) positioned below `anchor` (in `parent`'s surface
    /// coordinates), slid / flipped by the compositor to stay on screen. With `grab` (menus) it takes an explicit
    /// grab with the last button-press serial: the compositor dismisses it on an outside click, which closes `w`
    /// (popup_done -> AppWindow::requestClose). The compositor's configured size wins, as for toplevels.
    bool showPopup(AppWindow& w, AppWindow& parent, fuse::ui::Rect anchor, bool grab = true);
    /// The pixels last committed for `w` (RGBA like fuse::ui::Image).
    bool capture(const AppWindow& w, fuse::ui::Image& out) const;
    /// Has the compositor configured (mapped) `w`?
    bool configured(const AppWindow& w) const;
    /// True when `w` presents through a Vulkan swapchain (VK_KHR_wayland_surface), false for wl_shm.
    bool gpuPresents(const AppWindow& w) const;
    /// True when the GPU draw kernel (not the CPU-raster upload) draws `w`.
    bool gpuDraws(const AppWindow& w) const;
    /// "llvmpipe (LLVM ...)" etc. once a Vulkan presenter exists; empty otherwise.
    std::string_view gpuName() const;
    /// wl_display* (for a Vulkan surface or a toolkit sharing the connection).
    void* nativeDisplay() const;
    /// Blocks until the compositor processed all requests (tests).
    bool roundtrip();

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

/// "wayland" when $WAYLAND_DISPLAY is set (and not overridden by FUSE_UI_BACKEND=x11), else "x11" / "win32".
std::string_view preferredWindowSystem();

} // namespace fuse::app
