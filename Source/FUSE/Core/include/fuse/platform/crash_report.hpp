#pragma once

#include <functional>
#include <string>

#include <fuse/types.hpp>

namespace fuse::platform {

struct CrashReportContext {
    const char* message = nullptr;
    const char* file = nullptr;
    u32 line = 0;
};

using CrashReportCallback = std::function<void(const CrashReportContext& context)>;

/// Install the process crash handlers (B7.8). Idempotent; returns true when OS handlers are active.
///
/// POSIX: SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT handlers run on an alternate signal stack (so stack
/// overflow is reported), write a text crash report (signal, fault address, faulting pc, stack
/// frames with symbols where the binary exports them, the note set by setCrashContextNote, and the
/// process module map for offline symbolisation with addr2line), then chain to the previously
/// installed handler so the process still dies with the original signal.
/// Win32: an unhandled-exception filter writes a minidump (dbghelp, loaded at runtime) plus the
/// same text report.
bool installCrashHandlers();

/// Restore the handlers that were active before installCrashHandlers().
void shutdownCrashHandlers();

bool crashHandlersInstalled();

/// Directory the crash report is written to. Default: $FUSE_CRASH_DIR, else the working directory.
void setCrashReportDirectory(const char* directory);

/// Report file this process writes on a crash: <directory>/fuse_crash_<pid>.txt
/// (Win32 also writes <directory>/fuse_crash_<pid>.dmp).
std::string crashReportPath();

/// Free-form context copied into the next crash report (e.g. the failed assertion). Truncated to
/// 511 bytes; nullptr clears it. fuse::assertion::fatal() sets it before aborting.
void setCrashContextNote(const char* note);

void setCrashReportCallback(CrashReportCallback callback);

/// Manual report path for tests and non-fatal diagnostics.
void submitCrashReport(const CrashReportContext& context);

} // namespace fuse::platform
