// NativePlatform's Wayland backend (Linux / FreeBSD): what native.cpp dispatches to when the window system in use is
// Wayland (see NativeOptions::backend). The protocol work is in native_wayland.cpp (wayland_internal.hpp); this is the
// NativePlatform side: windows and their presenters (VK_KHR_wayland_surface swapchain, else wl_shm), the event pump,
// AppWindow window management, cursors, the clipboard, drops, IME, OS popups as xdg_popups.
//
// Differences from the X11 backend that the protocol imposes:
//  - a client cannot place, raise or activate a toplevel: WindowRequest::Position / Activate report false,
//    AppWindow::position() stays (0, 0) and NativePlatform::screenOrigin is (0, 0)
//  - hiding unmaps the surface (a null buffer); showing it again maps it where the compositor likes
//  - minimise is a request without a state: AppWindow::mode() never becomes Minimized
//  - nativeWindow() is the wl_surface*, nativeDisplay() the wl_display*
//  - postClick is delivered inside the process (nothing can post input to a surface)
//  - capture() reads the last committed wl_shm buffer (software) or the swapchain (Vulkan), not the screen
//  - decorations: server side through xdg-decoration when the compositor offers it; none otherwise (no client-side
//    frame is drawn here: wl::beginMove / beginResize are what one would call)
#pragma once

#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
#include "native_internal.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fuse::app::detail {

class WaylandPlatform {
public:
    /// Connects to the compositor and installs the event source. Null (and `err`) when there is none.
    static std::unique_ptr<WaylandPlatform> open(Application& app, const NativeOptions& o, std::string& err);
    ~WaylandPlatform();
    WaylandPlatform(const WaylandPlatform&) = delete;
    WaylandPlatform& operator=(const WaylandPlatform&) = delete;

    PresenterKind show(AppWindow& w);
    PresenterKind kind(const AppWindow& w) const;
    std::string_view gpuName() const;
    bool gpuDraws(const AppWindow& w) const;
    std::string_view lastError() const;
    bool setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex);
    bool capture(const AppWindow& w, fuse::ui::Image& out);
    void* nativeDisplay() const;
    unsigned long long nativeWindow(const AppWindow& w) const;
    bool setIcon(const AppWindow& w, std::span<const fuse::ui::Image> sizes);
    bool resizeWindow(const AppWindow& w, unsigned width, unsigned height);
    std::vector<int> iconSizes(const AppWindow& w) const;
    std::size_t popupCount(const AppWindow& w) const;
    unsigned long long popupWindow(const AppWindow& w, std::size_t i) const;
    fuse::ui::Rect popupScreenRect(const AppWindow& w, std::size_t i) const; ///< window-relative, device pixels
    bool capturePopup(const AppWindow& w, std::size_t i, fuse::ui::Image& out);
    float scale(const AppWindow& w) const;
    fuse::ui::Cursor cursor(const AppWindow& w) const;
    bool setClipboardText(std::string_view utf8);
    bool clipboardText(std::string& out);
    bool postClick(unsigned long long nativeWin, fuse::ui::Vec2 at);
    std::vector<DisplayInfo> displays() const;

    struct State;

private:
    explicit WaylandPlatform(std::unique_ptr<State> s);
    std::unique_ptr<State> m;
};

} // namespace fuse::app::detail
#endif
