#pragma once

#include <fuse/types.hpp>

namespace fuse::platform {

/// Process DPI awareness (B7.8 "DPI awareness — correct on all monitor configurations").
///
/// Windows: without an explicit opt-in the OS bitmap-stretches every window on monitors above
/// 96 DPI (blurry swapchain, wrong mouse coordinates). FUSE opts the process into
/// per-monitor-v2 awareness at startup, before any window exists. Other platforms report
/// `NotApplicable` (X11/Wayland/macOS/mobile expose scale through their window systems).
enum class DpiAwareness : u8 {
    NotApplicable, ///< Non-Windows target: nothing to configure.
    Unknown,       ///< Windows, but the awareness could not be queried.
    Unaware,       ///< DPI_AWARENESS_UNAWARE — OS scales the window (blurry).
    System,        ///< System-DPI aware (SetProcessDPIAware, Vista+).
    PerMonitor,    ///< Per-monitor v1 (Windows 8.1 shcore / 10 1607 context).
    PerMonitorV2,  ///< Per-monitor v2 (Windows 10 1703+): non-client + dialog scaling, WM_DPICHANGED.
};

/// Which Win32 entry point established the awareness in `enableHighDpiAwareness()`.
enum class DpiAwarenessApi : u8 {
    None,                           ///< Not called / non-Windows / every API failed.
    AlreadySet,                     ///< Awareness was fixed before FUSE ran (manifest or host).
    SetProcessDpiAwarenessContextV2, ///< user32 SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)
    SetProcessDpiAwarenessContextV1, ///< user32 SetProcessDpiAwarenessContext(PER_MONITOR_AWARE)
    SetProcessDpiAwareness,         ///< shcore SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)
    SetProcessDPIAware,             ///< user32 SetProcessDPIAware (system awareness only)
};

struct DpiAwarenessResult {
    DpiAwareness awareness = DpiAwareness::NotApplicable;
    DpiAwarenessApi api = DpiAwarenessApi::None;
    /// Win32 GetLastError() of the most preferred call that failed (0 if none failed).
    u32 lastError = 0;
};

/// Opt the process into the highest DPI awareness the OS supports: per-monitor v2, then
/// per-monitor v1, then shcore per-monitor, then system awareness. Idempotent; the first call's
/// result is cached. Called from `fuse::core::initialize()` and before the first owned HWND.
/// Returns `NotApplicable` on non-Windows targets.
DpiAwarenessResult enableHighDpiAwareness();

/// The awareness currently in effect for the calling thread (queried from the OS each call).
DpiAwareness currentDpiAwareness();

/// DPI of a native window (HWND on Win32; GetDpiForWindow, Windows 10 1607+), else the system
/// DPI, else 96. Non-Windows targets and null handles return 96.
u32 windowDpi(void* nativeWindow);

const char* dpiAwarenessName(DpiAwareness awareness);
const char* dpiAwarenessApiName(DpiAwarenessApi api);

} // namespace fuse::platform
