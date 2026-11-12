// fuse_app custom window chrome, Win32 side: window style per WindowFrame and the non-client messages (hit test,
// frame removal, title-bar button clicks). Called from the native window procedure.
#pragma once

#include "fuse/app/app.hpp"

#include <cstdint>

namespace fuse::app::detail {

#if defined(_WIN32)
/// WS_* style of a top-level window with this frame.
unsigned long chromeWin32Style(WindowFrame f);
/// After CreateWindowExW (and GWLP_USERDATA): installs the min / max / close hook and recomputes the frame.
void chromeWin32Attach(void* hwnd, AppWindow& w);
void chromeWin32Detach(AppWindow& w);
/// Handles WM_NCCALCSIZE / WM_NCHITTEST / WM_NC*BUTTON* for Borderless / Custom windows. True when handled
/// (`result` is then the window procedure's return value).
bool chromeWin32Message(void* hwnd, AppWindow& w, unsigned msg, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result);
#endif

} // namespace fuse::app::detail
