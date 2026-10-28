#pragma once

// Internal Win32 message -> app visibility mapping shared by the window pump and tests.
//
// WM_ACTIVATEAPP: nonzero wParam is Foreground, zero is Background.
// WM_SIZE: SIZE_MINIMIZED is Background; every other size code is Foreground.
// The pump applies a non-minimized WM_SIZE result only while that window is focused.
// Any other message leaves visibility unchanged.

#include <fuse/platform/lifecycle.hpp>

#include <optional>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fuse::platform {

[[nodiscard]] inline std::optional<AppVisibility> visibilityFromWin32(UINT msg, WPARAM wParam) {
    if (msg == WM_ACTIVATEAPP) {
        return wParam != 0 ? AppVisibility::Foreground : AppVisibility::Background;
    }
    if (msg == WM_SIZE) {
        if (wParam == SIZE_MINIMIZED) {
            return AppVisibility::Background;
        }
        return AppVisibility::Foreground;
    }
    return std::nullopt;
}

} // namespace fuse::platform

#endif
