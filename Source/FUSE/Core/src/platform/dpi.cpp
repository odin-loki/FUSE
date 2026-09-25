// Process DPI awareness (B7.8). Every Win32 DPI entry point is resolved at run time so the binary
// still starts on Windows versions (or Wine builds) that lack the newer ones.
#include <fuse/platform/dpi.hpp>

#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fuse::platform {

namespace {

std::once_flag g_enableOnce;
DpiAwarenessResult g_result;

#if defined(_WIN32)

// Declared locally: the SDK / MinGW headers only expose these for _WIN32_WINNT >= 0x0605/0x0A00.
using DpiContext = HANDLE;
const DpiContext kContextUnaware = reinterpret_cast<DpiContext>(static_cast<INT_PTR>(-1));
const DpiContext kContextSystemAware = reinterpret_cast<DpiContext>(static_cast<INT_PTR>(-2));
const DpiContext kContextPerMonitorAware = reinterpret_cast<DpiContext>(static_cast<INT_PTR>(-3));
const DpiContext kContextPerMonitorAwareV2 = reinterpret_cast<DpiContext>(static_cast<INT_PTR>(-4));

constexpr int kProcessPerMonitorDpiAware = 2; // PROCESS_DPI_AWARENESS::PROCESS_PER_MONITOR_DPI_AWARE
constexpr LONG kHResultAccessDenied = static_cast<LONG>(0x80070005L); // E_ACCESSDENIED

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DpiContext);
using GetThreadDpiAwarenessContextFn = DpiContext(WINAPI*)();
using AreDpiAwarenessContextsEqualFn = BOOL(WINAPI*)(DpiContext, DpiContext);
using GetAwarenessFromDpiAwarenessContextFn = int(WINAPI*)(DpiContext);
using SetProcessDpiAwarenessFn = LONG(WINAPI*)(int);
using GetProcessDpiAwarenessFn = LONG(WINAPI*)(HANDLE, int*);
using SetProcessDPIAwareFn = BOOL(WINAPI*)();
using IsProcessDPIAwareFn = BOOL(WINAPI*)();
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForSystemFn = UINT(WINAPI*)();

template <typename Fn>
Fn resolve(HMODULE module, const char* name) {
    if (module == nullptr) {
        return nullptr;
    }
    // Through a generic function pointer: FARPROC -> exact signature without -Wcast-function-type.
    return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(::GetProcAddress(module, name)));
}

HMODULE user32() {
    static HMODULE module = ::LoadLibraryExA("user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module != nullptr ? module : ::GetModuleHandleA("user32.dll");
}

HMODULE shcore() {
    static HMODULE module = [] {
        HMODULE m = ::LoadLibraryExA("shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m != nullptr ? m : ::LoadLibraryA("shcore.dll");
    }();
    return module;
}

DpiAwareness queryAwareness() {
    HMODULE u = user32();
    const auto getThreadContext = resolve<GetThreadDpiAwarenessContextFn>(u, "GetThreadDpiAwarenessContext");
    const auto contextsEqual = resolve<AreDpiAwarenessContextsEqualFn>(u, "AreDpiAwarenessContextsEqual");
    if (getThreadContext != nullptr && contextsEqual != nullptr) {
        const DpiContext context = getThreadContext();
        if (contextsEqual(context, kContextPerMonitorAwareV2)) {
            return DpiAwareness::PerMonitorV2;
        }
        if (contextsEqual(context, kContextPerMonitorAware)) {
            return DpiAwareness::PerMonitor;
        }
        if (contextsEqual(context, kContextSystemAware)) {
            return DpiAwareness::System;
        }
        if (contextsEqual(context, kContextUnaware)) {
            return DpiAwareness::Unaware;
        }
        const auto awarenessOf =
            resolve<GetAwarenessFromDpiAwarenessContextFn>(u, "GetAwarenessFromDpiAwarenessContext");
        if (awarenessOf != nullptr) {
            switch (awarenessOf(context)) {
            case 0: return DpiAwareness::Unaware;
            case 1: return DpiAwareness::System;
            case 2: return DpiAwareness::PerMonitor;
            default: break;
            }
        }
    }
    const auto getProcessAwareness = resolve<GetProcessDpiAwarenessFn>(shcore(), "GetProcessDpiAwareness");
    if (getProcessAwareness != nullptr) {
        int value = -1;
        if (getProcessAwareness(nullptr, &value) >= 0) {
            switch (value) {
            case 0: return DpiAwareness::Unaware;
            case 1: return DpiAwareness::System;
            case 2: return DpiAwareness::PerMonitor;
            default: break;
            }
        }
    }
    const auto isAware = resolve<IsProcessDPIAwareFn>(u, "IsProcessDPIAware");
    if (isAware != nullptr) {
        return isAware() ? DpiAwareness::System : DpiAwareness::Unaware;
    }
    return DpiAwareness::Unknown;
}

DpiAwarenessResult enableOnce() {
    DpiAwarenessResult result;
    result.awareness = DpiAwareness::Unknown;
    HMODULE u = user32();

    // Windows 10 1703+: per-monitor v2, then v1 context. ERROR_ACCESS_DENIED means the awareness
    // was already fixed (application manifest, or an embedding host called first): keep it.
    const auto setContext = resolve<SetProcessDpiAwarenessContextFn>(u, "SetProcessDpiAwarenessContext");
    if (setContext != nullptr) {
        if (setContext(kContextPerMonitorAwareV2)) {
            result.api = DpiAwarenessApi::SetProcessDpiAwarenessContextV2;
            result.awareness = queryAwareness();
            return result;
        }
        const DWORD error = ::GetLastError();
        result.lastError = error;
        if (error == ERROR_ACCESS_DENIED) {
            result.api = DpiAwarenessApi::AlreadySet;
            result.awareness = queryAwareness();
            return result;
        }
        // ERROR_INVALID_PARAMETER: the OS predates the v2 context (1607, or Wine 9.0) — try v1.
        if (setContext(kContextPerMonitorAware)) {
            result.api = DpiAwarenessApi::SetProcessDpiAwarenessContextV1;
            result.awareness = queryAwareness();
            return result;
        }
        if (::GetLastError() == ERROR_ACCESS_DENIED) {
            result.api = DpiAwarenessApi::AlreadySet;
            result.awareness = queryAwareness();
            return result;
        }
    }

    // Windows 8.1: shcore per-monitor (v1).
    const auto setAwareness = resolve<SetProcessDpiAwarenessFn>(shcore(), "SetProcessDpiAwareness");
    if (setAwareness != nullptr) {
        const LONG hr = setAwareness(kProcessPerMonitorDpiAware);
        if (hr >= 0) {
            result.api = DpiAwarenessApi::SetProcessDpiAwareness;
            result.awareness = queryAwareness();
            return result;
        }
        if (result.lastError == 0u) {
            result.lastError = static_cast<u32>(hr);
        }
        if (hr == kHResultAccessDenied) {
            result.api = DpiAwarenessApi::AlreadySet;
            result.awareness = queryAwareness();
            return result;
        }
    }

    // Vista / 7: system awareness only.
    const auto setAware = resolve<SetProcessDPIAwareFn>(u, "SetProcessDPIAware");
    if (setAware != nullptr && setAware()) {
        result.api = DpiAwarenessApi::SetProcessDPIAware;
    }
    result.awareness = queryAwareness();
    return result;
}

#endif

} // namespace

DpiAwarenessResult enableHighDpiAwareness() {
#if defined(_WIN32)
    std::call_once(g_enableOnce, [] { g_result = enableOnce(); });
    return g_result;
#else
    std::call_once(g_enableOnce, [] { g_result = DpiAwarenessResult{}; });
    return g_result;
#endif
}

DpiAwareness currentDpiAwareness() {
#if defined(_WIN32)
    return queryAwareness();
#else
    return DpiAwareness::NotApplicable;
#endif
}

u32 windowDpi(void* nativeWindow) {
#if defined(_WIN32)
    HMODULE u = user32();
    if (nativeWindow != nullptr) {
        const auto forWindow = resolve<GetDpiForWindowFn>(u, "GetDpiForWindow");
        if (forWindow != nullptr) {
            const UINT dpi = forWindow(static_cast<HWND>(nativeWindow));
            if (dpi != 0u) {
                return dpi;
            }
        }
    }
    const auto forSystem = resolve<GetDpiForSystemFn>(u, "GetDpiForSystem");
    if (forSystem != nullptr) {
        const UINT dpi = forSystem();
        if (dpi != 0u) {
            return dpi;
        }
    }
    HDC screen = ::GetDC(nullptr);
    if (screen != nullptr) {
        const int dpi = ::GetDeviceCaps(screen, LOGPIXELSX);
        ::ReleaseDC(nullptr, screen);
        if (dpi > 0) {
            return static_cast<u32>(dpi);
        }
    }
    return 96u;
#else
    (void)nativeWindow;
    return 96u;
#endif
}

const char* dpiAwarenessName(DpiAwareness awareness) {
    switch (awareness) {
    case DpiAwareness::NotApplicable: return "not-applicable";
    case DpiAwareness::Unknown: return "unknown";
    case DpiAwareness::Unaware: return "unaware";
    case DpiAwareness::System: return "system";
    case DpiAwareness::PerMonitor: return "per-monitor";
    case DpiAwareness::PerMonitorV2: return "per-monitor-v2";
    }
    return "unknown";
}

const char* dpiAwarenessApiName(DpiAwarenessApi api) {
    switch (api) {
    case DpiAwarenessApi::None: return "none";
    case DpiAwarenessApi::AlreadySet: return "already-set";
    case DpiAwarenessApi::SetProcessDpiAwarenessContextV2: return "SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)";
    case DpiAwarenessApi::SetProcessDpiAwarenessContextV1: return "SetProcessDpiAwarenessContext(PER_MONITOR_AWARE)";
    case DpiAwarenessApi::SetProcessDpiAwareness: return "SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)";
    case DpiAwarenessApi::SetProcessDPIAware: return "SetProcessDPIAware";
    }
    return "none";
}

} // namespace fuse::platform
