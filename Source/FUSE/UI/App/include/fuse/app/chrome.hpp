// fuse_app custom window chrome (plan §4.13): a FUSE UI title bar for WindowFrame::Borderless / Custom windows and the
// hit test the OS backend asks for every pointer position: which pixels drag the window (caption), resize it (edges
// and corners, Custom frame, not while maximised) or are its min / max / close buttons.
//
//   auto& w = app.createWindow({.title = "Tool", .frame = fuse::app::WindowFrame::Custom});
//   fuse::app::titleBar(w, w.ui().root(), "Tool");   // drag area + min / max / close
//   native.show(w);
//
// The buttons call AppWindow::command (minimize / maximise or restore / tryClose), which works headless too.
// X11: _MOTIF_WM_HINTS asks the window manager for no decorations; a press on the caption / an edge hands the drag to
//      the window manager with _NET_WM_MOVERESIZE, and without a window manager the backend moves / resizes the
//      window itself while the button is held. A caption double click toggles maximise.
// Win32: Custom keeps WS_OVERLAPPEDWINDOW (snap, maximise animations) with the whole window as client area
//      (WM_NCCALCSIZE) and answers WM_NCHITTEST from chromeHitTest(); Borderless is WS_POPUP with the same hit test.
//      Compiled with MinGW only: not run (see docs/ui/KNOWN_GAPS.md).
// Wayland: no decoration is asked for; a press on the caption / an edge is handed to the compositor with
//      xdg_toplevel.move / resize (the compositor then owns the pointer until the drag ends), a caption double click
//      toggles maximise. A System-frame window the compositor does not decorate gets this chrome by itself
//      (NativeOptions::clientDecorations).
// macOS / iOS / Android: the frame kind is ignored (the system frame stays); the title bar is an ordinary widget row.
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
/// "titlebar.title", "titlebar.min", "titlebar.max", "titlebar.close". Styled by the classes "fuse-titlebar",
/// "fuse-titlebar-btn" and "fuse-titlebar-close" (theme tokens). Returns the bar.
fuse::ui::NodeHandle titleBar(AppWindow& w, fuse::ui::NodeHandle parent, std::string_view title, fuse::ui::f32 height = 32.f);

} // namespace fuse::app
