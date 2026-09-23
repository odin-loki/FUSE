// B7.8 DPI awareness gate (FUSE_MASTER_PLAN B7.8 "DPI awareness — correct on all monitor
// configurations"): fuse::core::initialize() opts the process into per-monitor-v2 awareness before
// any window exists, falling back through per-monitor v1 / shcore / system awareness.
//
// Checks (Windows builds; the Linux/macOS build of this file checks the NotApplicable contract):
//   - after core::initialize(), the OS reports the awareness enableHighDpiAwareness() claims, and
//     it is per-monitor v2 whenever SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) succeeded;
//     at least per-monitor awareness whenever user32/shcore expose any per-monitor API;
//   - the result is cached (second call identical) and an owned native window is created
//     per-monitor-v2 (GetWindowDpiAwarenessContext) with a sane DPI (GetDpiForWindow);
//   - a child process whose awareness was fixed before FUSE ran (as an application manifest or
//     an embedding host would) is left alone: api == AlreadySet, awareness == the preset one.
// Real multi-monitor / mixed-scale behaviour (WM_DPICHANGED on a monitor move) stays manual.
#include <fuse/core/init.hpp>
#include <fuse/platform/dpi.hpp>
#include <fuse/platform/window.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using fuse::platform::DpiAwareness;
using fuse::platform::DpiAwarenessApi;

namespace {

int g_failures = 0;
int g_checks = 0;

void expectTrue(bool condition, const char* message) {
    ++g_checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

#if defined(_WIN32)

using DpiContext = HANDLE;
const DpiContext kContextSystemAware = reinterpret_cast<DpiContext>(static_cast<INT_PTR>(-2));
const DpiContext kContextPerMonitorAwareV2 = reinterpret_cast<DpiContext>(static_cast<INT_PTR>(-4));

template <typename Fn>
Fn user32Proc(const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(::GetProcAddress(::GetModuleHandleA("user32.dll"), name)));
}

bool hasPerMonitorApi() {
    HMODULE shcore = ::LoadLibraryA("shcore.dll");
    const bool shcoreApi = shcore != nullptr && ::GetProcAddress(shcore, "SetProcessDpiAwareness") != nullptr;
    return user32Proc<void (*)()>("SetProcessDpiAwarenessContext") != nullptr || shcoreApi;
}

/// Child: awareness preset to system-aware before FUSE runs; exit code encodes what FUSE saw.
int runPresetChild() {
    using SetContextFn = BOOL(WINAPI*)(DpiContext);
    const auto setContext = user32Proc<SetContextFn>("SetProcessDpiAwarenessContext");
    if (setContext == nullptr || !setContext(kContextSystemAware)) {
        return 77; // cannot preset (pre-1703 Windows / Wine without the API)
    }
    fuse::core::initialize();
    const auto result = fuse::platform::enableHighDpiAwareness();
    std::printf("  preset child: api %s, awareness %s, last error %u\n",
                fuse::platform::dpiAwarenessApiName(result.api),
                fuse::platform::dpiAwarenessName(result.awareness), result.lastError);
    fuse::core::shutdown();
    return static_cast<int>(result.api) * 16 + static_cast<int>(result.awareness);
}

DWORD spawnPresetChild() {
    wchar_t exe[MAX_PATH * 2];
    ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    std::wstring command = std::wstring(L"\"") + exe + L"\" --dpi-preset-child";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return 0xFFFFFFFFu;
    }
    ::WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0xFFFFFFFFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return code;
}

void checkWindows() {
    const DpiAwareness before = fuse::platform::currentDpiAwareness();
    fuse::core::initialize(); // startup path under test
    const DpiAwareness after = fuse::platform::currentDpiAwareness();
    const auto result = fuse::platform::enableHighDpiAwareness();
    std::printf("  awareness before init: %s; after init: %s via %s (last error %u)\n",
                fuse::platform::dpiAwarenessName(before), fuse::platform::dpiAwarenessName(after),
                fuse::platform::dpiAwarenessApiName(result.api), result.lastError);

    expectTrue(result.awareness == after, "reported awareness matches the OS query after core::initialize()");
    expectTrue(result.api != DpiAwarenessApi::None, "some DPI awareness API succeeded");
    if (result.api == DpiAwarenessApi::SetProcessDpiAwarenessContextV2) {
        expectTrue(after == DpiAwareness::PerMonitorV2,
                   "SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) succeeded -> per-monitor v2");
    }
    if (hasPerMonitorApi()) {
        expectTrue(after == DpiAwareness::PerMonitorV2 || after == DpiAwareness::PerMonitor,
                   "per-monitor API available -> process is per-monitor aware");
    }
    // Per-monitor v2 is required wherever the OS knows the context (Windows 10 1703+). Wine 9.0
    // exports SetProcessDpiAwarenessContext but rejects PER_MONITOR_AWARE_V2 as invalid, so there
    // the v1 fallback is the expected (and checked) outcome.
    using IsValidContextFn = BOOL(WINAPI*)(DpiContext);
    const auto isValidContext = user32Proc<IsValidContextFn>("IsValidDpiAwarenessContext");
    const bool osKnowsV2 = isValidContext != nullptr && isValidContext(kContextPerMonitorAwareV2) != FALSE;
    std::printf("  OS accepts DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2: %s\n", osKnowsV2 ? "yes" : "no");
    if (osKnowsV2) {
        expectTrue(after == DpiAwareness::PerMonitorV2, "per-monitor-v2 context supported -> process is per-monitor v2");
    }
    const auto again = fuse::platform::enableHighDpiAwareness();
    expectTrue(again.api == result.api && again.awareness == result.awareness, "enableHighDpiAwareness is idempotent");

    fuse::platform::WindowDesc desc;
    desc.width = 640;
    desc.height = 360;
    desc.createNative = true;
    fuse::platform::Window window(desc);
    void* hwnd = window.nativeHandle().value;
    expectTrue(hwnd != nullptr, "owned native window created");
    const fuse::u32 dpi = fuse::platform::windowDpi(hwnd);
    std::printf("  window dpi: %u (scale %.2f)\n", dpi, static_cast<double>(dpi) / 96.0);
    expectTrue(dpi >= 72u && dpi <= 960u, "window DPI in a sane range");

    using GetWindowContextFn = DpiContext(WINAPI*)(HWND);
    using ContextsEqualFn = BOOL(WINAPI*)(DpiContext, DpiContext);
    const auto windowContext = user32Proc<GetWindowContextFn>("GetWindowDpiAwarenessContext");
    const auto equal = user32Proc<ContextsEqualFn>("AreDpiAwarenessContextsEqual");
    if (hwnd != nullptr && windowContext != nullptr && equal != nullptr && after == DpiAwareness::PerMonitorV2) {
        expectTrue(equal(windowContext(static_cast<HWND>(hwnd)), kContextPerMonitorAwareV2) != FALSE,
                   "owned window created with the per-monitor-v2 awareness context");
    } else {
        std::printf("  window awareness context not checked (API missing or process not per-monitor v2)\n");
    }

    const DWORD preset = spawnPresetChild();
    if (preset == 77u) {
        std::printf("  preset child: cannot preset awareness on this system (skipped)\n");
    } else {
        const DWORD expected = static_cast<DWORD>(DpiAwarenessApi::AlreadySet) * 16u +
                               static_cast<DWORD>(DpiAwareness::System);
        std::printf("  preset child exit code %lu (expected %lu)\n", preset, expected);
        expectTrue(preset == expected,
                   "awareness preset before FUSE (manifest/host) is kept: AlreadySet + system-aware");
    }
    fuse::core::shutdown();
}

#else

void checkOther() {
    fuse::core::initialize();
    const auto result = fuse::platform::enableHighDpiAwareness();
    expectTrue(result.awareness == DpiAwareness::NotApplicable && result.api == DpiAwarenessApi::None,
               "non-Windows: DPI awareness is not applicable");
    expectTrue(fuse::platform::currentDpiAwareness() == DpiAwareness::NotApplicable,
               "non-Windows: current awareness not applicable");
    expectTrue(fuse::platform::windowDpi(nullptr) == 96u, "non-Windows: windowDpi defaults to 96");
    fuse::core::shutdown();
}

#endif

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc >= 2 && std::strcmp(argv[1], "--dpi-preset-child") == 0) {
        return runPresetChild();
    }
    checkWindows();
#else
    (void)argc;
    (void)argv;
    checkOther();
#endif
    std::printf("fuse_core_b7_dpi_awareness: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
