// fuse_app custom window chrome (plan §4.13 Windows): a FUSE UI title bar for WindowFrame::Borderless / Custom windows
// and the hit test the OS backend asks for every pointer position (Win32 WM_NCHITTEST): which pixels drag the window
// (caption), resize it (edges and corners, Custom frame, not while maximised) or act as its min / max / close buttons.
//
//   auto& w = app.createWindow({.title = "Tool", .frame = fuse::app::WindowFrame::Custom});
//   fuse::app::titleBar(w, w.ui().root(), "Tool");   // drag area + min / max / close
//   native.show(w);
//
// Not yet: X11 (the window keeps its WM frame there; _MOTIF_WM_HINTS + _NET_WM_MOVERESIZE would do it).
#pragma once

#include "fuse/app/app.hpp"

#include <string_view>

namespace fuse::app {

enum class HitArea : fuse::ui::u8 {
    Client, Caption,
    Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight,
    Minimize, Maximize, Close,
};
std::string_view hitAreaName(HitArea a);

/// What the window-local point `p` (logical px) is: a resize edge (Custom frame, not maximised), a title-bar button,
/// the caption (the caption node or a non-interactive child of it) or plain client area. System frames: Client.
HitArea chromeHitTest(const AppWindow& w, fuse::ui::Vec2 p);

/// A title bar (row: title label, flexible drag area, minimise / maximise / close tool buttons) appended to `parent`;
/// fills w.chrome() so chromeHitTest() knows it and wires the buttons to AppWindow::command(). Ids: "titlebar",
/// "titlebar.min", "titlebar.max", "titlebar.close". Returns the bar.
fuse::ui::NodeHandle titleBar(AppWindow& w, fuse::ui::NodeHandle parent, std::string_view title, fuse::ui::f32 height = 32.f);

} // namespace fuse::app
