// fuse_app Wayland backend (G2): native libwayland-client, loaded at run time through the OS seam (no build-time
// wayland dependency, like the X11 backend). xdg-shell toplevels, wl_shm software presentation of the CPU raster,
// wl_seat pointer + keyboard (XKB keymaps via a runtime-loaded libxkbcommon), and system drag and drop both ways
// over wl_data_device (AppWindow::onExternalDrop / beginExternalDrag).
//
//   fuse::app::Application app{{.name = "hello"}};
//   fuse::app::WaylandPlatform wl(app);            // connects to $WAYLAND_DISPLAY
//   if (wl.available()) wl.show(app.createWindow({.title = "Hello"}));
//   else { /* fall back to NativePlatform (X11 / XWayland) */ }
//
// Declare WaylandPlatform after the Application. Vulkan presentation (VK_KHR_wayland_surface) is not wired yet.
#pragma once
#include "fuse/app/app.hpp"
#include <memory>
#include <string_view>

namespace fuse::app {

class WaylandPlatform {
public:
    explicit WaylandPlatform(Application& app, const char* displayName = nullptr);
    ~WaylandPlatform();
    WaylandPlatform(const WaylandPlatform&) = delete;
    WaylandPlatform& operator=(const WaylandPlatform&) = delete;

    /// Connected, with wl_compositor + wl_shm + xdg_wm_base.
    bool available() const;
    std::string_view lastError() const;
    /// xdg_toplevel for `w`, presented through wl_shm. False when unavailable.
    bool show(AppWindow& w);
    /// The pixels last committed for `w` (RGBA like fuse::ui::Image).
    bool capture(const AppWindow& w, fuse::ui::Image& out) const;
    /// Has the compositor configured (mapped) `w`?
    bool configured(const AppWindow& w) const;
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
