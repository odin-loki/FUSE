// fuse_app platform services shared by the window backends (native.cpp) and their OS implementations
// (platform_win32.cpp / platform_posix.cpp).
#pragma once

#include "fuse/app/native.hpp"

#include <cstdint>

namespace fuse::platform {
struct EventV2;
}

namespace fuse::app::detail {

/// Per-window state of the platform services (kept in the backend's window record).
struct ServiceState {
    fuse::ui::CursorShape cursor = fuse::ui::CursorShape::Auto; ///< last applied to the OS
    bool darkTitle = false;
};

SystemTheme queryTheme(const NativeOptions& o);

#if defined(_WIN32)
void winEnableDpiAwareness();
float winWindowScale(void* hwnd);
/// Window size (outer) for a client area of `w` x `h` device pixels at `dpi` (AdjustWindowRectExForDpi).
void winOuterSize(void* hwnd, unsigned w, unsigned h, unsigned dpi, int& ow, int& oh);
bool winSetDarkTitleBar(void* hwnd, bool dark);
SystemTheme winQueryTheme();
std::vector<DisplayInfo> winDisplays();
/// Shows the shape (SetCursor) and records it in `st`.
void winApplyCursor(ServiceState& st, fuse::ui::CursorShape c);
/// WM_SETTINGCHANGE / WM_THEMECHANGED / WM_DWMCOLORIZATIONCOLORCHANGED / WM_SYSCOLORCHANGE.
bool winIsThemeMessage(unsigned msg);
/// WM_POINTERDOWN / UPDATE / UP of a touch or pen contact -> Touch* / Pen* event in client pixels. False for mouse /
/// touchpad pointers (they arrive as mouse messages) and other messages.
bool winPointerEvent(void* hwnd, unsigned msg, std::uintptr_t wp, std::intptr_t lp, fuse::platform::EventV2& out);
#endif

} // namespace fuse::app::detail
