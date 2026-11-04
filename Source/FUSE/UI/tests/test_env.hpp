// Where a FUSE UI gate runs: whether wall-clock budgets apply, and whether it runs inside Wine (header-only, no
// widget dependencies, so app / startup gates can use it too).
#pragma once

#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near // empty 16-bit leftovers from minwindef.h: gates use these names
#undef far
#endif

namespace fuse::ui::test {

/// Wall-clock budgets are enforced unless FUSE_INSTRUMENTED_RUN is set to something other than "0" (the Wine runner
/// cmake/toolchains/fuse-wine-run.sh sets "wine", valgrind runs "valgrind"; the same switch as
/// fuse::core::timingBudgetsEnforced()). Gates then report the timing without failing on it.
inline bool timingBudgetsEnforced() {
    const char* e = std::getenv("FUSE_INSTRUMENTED_RUN");
    return !e || !*e || std::string_view(e) == "0";
}
inline const char* instrumentedRun() {
    const char* e = std::getenv("FUSE_INSTRUMENTED_RUN");
    return e && *e ? e : "";
}

/// True inside Wine (its ntdll exports wine_get_version): no Windows fonts (Segoe UI, Arial), no pointer stack for touch.
inline bool runningUnderWine() {
#if defined(_WIN32)
    const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

} // namespace fuse::ui::test
