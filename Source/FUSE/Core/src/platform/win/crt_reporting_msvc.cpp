// MSVC CRT (cl.exe and clang-cl, static or DLL UCRT): no modal dialogs from the C runtime.
//
// Out of the box the Debug CRT reports assert(), _ASSERTE, STL iterator-debugging failures
// (_ITERATOR_DEBUG_LEVEL=2 bounds checks) and abort() through _CrtDbgReport, which opens an
// Abort/Retry/Ignore message box and blocks until someone clicks it: a failing test on a CI runner
// or a crashing tool in a batch cook would hang until its timeout. The UCRT's abort() also defaults
// to _CALL_REPORTFAULT, i.e. __fastfail(FAST_FAIL_FATAL_APP_EXIT) straight to Windows Error
// Reporting (exit code 0xC0000409) after the SIGABRT handler returns.
//
// Unless a debugger is attached (then the dialogs / break-into-debugger behaviour stay useful),
// this routes every CRT report to stderr and makes abort() run the SIGABRT path (FUSE crash handler:
// minidump + report) and then exit with code 3 — the same observable behaviour as the MinGW /
// msvcrt build and as abort() on POSIX (non-zero, crash artifacts written by FUSE itself).
//
// fuse_core is a static library, so nothing would pull this translation unit into an executable;
// Source/FUSE/Core/CMakeLists.txt adds /INCLUDE:fuse_msvc_crt_reporting_anchor to fuse_core's
// INTERFACE link options on MSVC-style toolchains so every program linking fuse_core gets it.
#if defined(_MSC_VER) && defined(_WIN32)

#include <crtdbg.h>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
// Referenced by name from the linker command line (/INCLUDE); keep the symbol external.
int fuse_msvc_crt_reporting_anchor = 0;
}

namespace {

struct CrtReportingSetup {
    CrtReportingSetup() {
        if (::IsDebuggerPresent()) {
            return;
        }
        _set_abort_behavior(0u, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        // No-ops against the release CRT (_DEBUG unset): the macros expand to nothing there.
        (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        fuse_msvc_crt_reporting_anchor = 1;
    }
};

const CrtReportingSetup g_crtReportingSetup;

} // namespace

#endif // _MSC_VER && _WIN32
