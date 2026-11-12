// fuse_app_a11y: screen-reader access for a native fuse_app window (plan §4.14, D7). Owns a fuse_ui_a11y Bridge and
// the AccessKit adapter of the OS (Windows: UI Automation through WM_GETOBJECT; Linux: AT-SPI), and keeps them in
// step with the window from the application's event pump: before it blocks, the last frame's changes are pushed
// (tree, focus and property events); after it wakes, actions from assistive technology run on the UI thread.
//
//   fuse::app::NativePlatform native(app);
//   native.show(win);
//   fuse::app::WindowAccessibility a11y(native, win); // declare after the NativePlatform (destroyed first)
//
// Without AccessKit in the build (FUSE_UI_ACCESSKIT_DIR / FUSE_UI_ACCESSKIT_BUILD) the bridge still runs and
// active() is false.
#pragma once

#include "fuse/app/native.hpp"
#include "fuse/ui/a11y/a11y.hpp"

#include <memory>

namespace fuse::app {

class WindowAccessibility {
public:
    /// `w` must be shown (NativePlatform::show) for a platform adapter to attach.
    WindowAccessibility(NativePlatform& native, AppWindow& w);
    ~WindowAccessibility();
    WindowAccessibility(const WindowAccessibility&) = delete;
    WindowAccessibility& operator=(const WindowAccessibility&) = delete;

    /// True when an OS adapter is attached (AccessKit linked, window shown, platform supported).
    bool active() const;
    fuse::ui::a11y::Bridge& bridge();
    /// Applies queued actions and pushes changes now (the event pump also does this every loop iteration).
    void update();
    /// Overrides the window focus state the adapter reports (it follows WM_SETFOCUS / WM_KILLFOCUS on its own).
    void setWindowFocused(bool focused);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace fuse::app
