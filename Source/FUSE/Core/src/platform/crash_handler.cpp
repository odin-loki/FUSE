// Process crash handlers (B7.8 platform hardening).
//
// Everything reachable from the signal handler / exception filter is async-signal-safe: fixed
// static buffers, open/write/close (CreateFile/WriteFile on Win32), no heap, no locks, no stdio. Configuration (directory, note)
// is copied into static storage from normal code before a crash can use it.
#include <fuse/platform/crash_report.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <csignal>
#define FUSE_CRASH_WIN32 1
#elif (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))
#include <ucontext.h>
#define FUSE_CRASH_HAS_UCONTEXT_PC 1
#endif
#if (defined(__GLIBC__) || defined(__APPLE__)) && defined(__has_include)
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define FUSE_CRASH_HAS_BACKTRACE 1
#endif
#endif
#define FUSE_CRASH_POSIX 1
#endif

namespace fuse::platform {

namespace {

constexpr usize kPathCapacity = 1024u;
constexpr usize kNoteCapacity = 512u;
constexpr int kMaxFrames = 64;

std::atomic<bool> g_installed{false};
char g_directory[kPathCapacity] = {};
bool g_directorySet = false;
char g_note[kNoteCapacity] = {};

// ---- async-signal-safe string building ---------------------------------------------------------

struct Buf {
    char* data;
    usize cap;
    usize len;

    void put(const char* s) {
        if (s == nullptr) {
            return;
        }
        while (*s != '\0' && len + 1u < cap) {
            data[len++] = *s++;
        }
        data[len] = '\0';
    }
    void putDec(u64 value) {
        char tmp[24];
        int n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        } while (value != 0u && n < 24);
        while (n > 0 && len + 1u < cap) {
            data[len++] = tmp[--n];
        }
        data[len] = '\0';
    }
    void putHex(u64 value) {
        static const char kDigits[] = "0123456789abcdef";
        put("0x");
        for (int shift = 60; shift >= 0; shift -= 4) {
            if (len + 1u >= cap) {
                break;
            }
            data[len++] = kDigits[(value >> shift) & 0xFu];
        }
        data[len] = '\0';
    }
};

void resolveDefaultDirectory() {
    if (g_directorySet) {
        return;
    }
    const char* env = std::getenv("FUSE_CRASH_DIR");
    Buf b{g_directory, sizeof(g_directory), 0u};
    b.put((env != nullptr && env[0] != '\0') ? env : ".");
    g_directorySet = true;
}

void buildReportPath(Buf& out, u64 pid, const char* extension) {
    out.put(g_directory[0] != '\0' ? g_directory : ".");
    out.put("/fuse_crash_");
    out.putDec(pid);
    out.put(extension);
}

#if defined(FUSE_CRASH_POSIX)

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
constexpr int kSignalCount = static_cast<int>(sizeof(kSignals) / sizeof(kSignals[0]));

struct sigaction g_previous[kSignalCount];
std::atomic<int> g_handling{0};

constexpr usize kAltStackBytes = 64u * 1024u;
alignas(16) char g_altStack[kAltStackBytes];
bool g_ownsAltStack = false;

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

int signalIndex(int sig) {
    for (int i = 0; i < kSignalCount; ++i) {
        if (kSignals[i] == sig) {
            return i;
        }
    }
    return -1;
}

void writeAll(int fd, const char* data, usize len) {
    while (len > 0u) {
        const ssize_t n = ::write(fd, data, len);
        if (n <= 0) {
            return;
        }
        data += n;
        len -= static_cast<usize>(n);
    }
}

void writeStr(int fd, const char* s) {
    writeAll(fd, s, std::strlen(s));
}

u64 faultingPc(void* context) {
#if defined(FUSE_CRASH_HAS_UCONTEXT_PC) && defined(__x86_64__)
    if (context != nullptr) {
        return static_cast<u64>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
    }
#elif defined(FUSE_CRASH_HAS_UCONTEXT_PC) && defined(__aarch64__)
    if (context != nullptr) {
        return static_cast<u64>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
    }
#else
    (void)context;
#endif
    return 0u;
}

u64 currentThreadId() {
#if defined(__linux__)
    return static_cast<u64>(::syscall(SYS_gettid));
#else
    return 0u;
#endif
}

void writeReport(int fd, int sig, siginfo_t* info, void* context) {
    char line[256];
    Buf b{line, sizeof(line), 0u};

    writeStr(fd, "FUSE crash report\n");
    b.put("signal: ");
    b.putDec(static_cast<u64>(sig));
    b.put(" ");
    b.put(signalName(sig));
    b.put("\ncode: ");
    const int code = info != nullptr ? info->si_code : 0;
    if (code < 0) {
        b.put("-");
        b.putDec(static_cast<u64>(-static_cast<s64>(code)));
    } else {
        b.putDec(static_cast<u64>(code));
    }
    b.put("\nfault_address: ");
    if (code > 0 && info != nullptr) {
        b.putHex(reinterpret_cast<u64>(info->si_addr));
    } else {
        b.put("none (sent signal)");
    }
    b.put("\npc: ");
    b.putHex(faultingPc(context));
    b.put("\npid: ");
    b.putDec(static_cast<u64>(::getpid()));
    b.put("\ntid: ");
    b.putDec(currentThreadId());
    b.put("\n");
    writeAll(fd, line, b.len);

    writeStr(fd, "note: ");
    writeAll(fd, g_note, ::strnlen(g_note, sizeof(g_note)));
    writeStr(fd, "\n");

#if defined(FUSE_CRASH_HAS_BACKTRACE)
    void* frames[kMaxFrames];
    const int count = ::backtrace(frames, kMaxFrames);
    writeStr(fd, "frames:\n");
    for (int i = 0; i < count; ++i) {
        b.len = 0u;
        b.put("  ");
        b.putHex(reinterpret_cast<u64>(frames[i]));
        b.put("\n");
        writeAll(fd, line, b.len);
    }
    writeStr(fd, "symbols:\n");
    ::backtrace_symbols_fd(frames, count, fd);
#else
    writeStr(fd, "frames:\nsymbols:\n");
#endif

    // Module map: lets addr2line symbolise the raw frames offline (executables need not export
    // their symbols, and shipping binaries keep them in a separate debug file).
    writeStr(fd, "maps:\n");
    const int maps = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps >= 0) {
        char chunk[4096];
        for (;;) {
            const ssize_t n = ::read(maps, chunk, sizeof(chunk));
            if (n <= 0) {
                break;
            }
            writeAll(fd, chunk, static_cast<usize>(n));
        }
        ::close(maps);
    }
    writeStr(fd, "end\n");
}

void crashSignalHandler(int sig, siginfo_t* info, void* context) {
    const int index = signalIndex(sig);
    int expected = 0;
    if (!g_handling.compare_exchange_strong(expected, 1)) {
        // Another thread is already writing the report (or the handler itself faulted): give the
        // first report time to finish, then die with the default action.
        ::sleep(2);
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return;
    }

    char path[kPathCapacity + 64];
    Buf p{path, sizeof(path), 0u};
    buildReportPath(p, static_cast<u64>(::getpid()), ".txt");
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        writeReport(fd, sig, info, context);
        ::close(fd);
    }

    char msg[kPathCapacity + 128];
    Buf m{msg, sizeof(msg), 0u};
    m.put("FUSE: fatal ");
    m.put(signalName(sig));
    m.put(fd >= 0 ? ", crash report written to " : ", could not write crash report ");
    m.put(path);
    m.put("\n");
    writeAll(STDERR_FILENO, msg, m.len);

    // Chain: restore whatever handled this signal before us. A hardware fault re-executes the
    // faulting instruction on return and reaches that handler with the original siginfo; a sent
    // signal (abort(), kill) is re-raised and delivered once this handler returns.
    if (index >= 0) {
        ::sigaction(sig, &g_previous[index], nullptr);
    } else {
        ::signal(sig, SIG_DFL);
    }
    if (info == nullptr || info->si_code <= 0) {
        ::raise(sig);
    }
}

bool installOs() {
#if defined(FUSE_CRASH_HAS_BACKTRACE)
    // The first backtrace() call may dlopen the unwinder (heap + locks); do it now, not mid-crash.
    void* prime[2];
    (void)::backtrace(prime, 2);
#endif

    stack_t current{};
    if (::sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) != 0) {
        stack_t alt{};
        alt.ss_sp = g_altStack;
        alt.ss_size = kAltStackBytes;
        alt.ss_flags = 0;
        g_ownsAltStack = ::sigaltstack(&alt, nullptr) == 0;
    }

    struct sigaction action {};
    action.sa_sigaction = &crashSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    bool ok = true;
    for (int i = 0; i < kSignalCount; ++i) {
        ok = ::sigaction(kSignals[i], &action, &g_previous[i]) == 0 && ok;
    }
    g_handling.store(0);
    return ok;
}

void shutdownOs() {
    for (int i = 0; i < kSignalCount; ++i) {
        ::sigaction(kSignals[i], &g_previous[i], nullptr);
    }
    if (g_ownsAltStack) {
        stack_t disable{};
        disable.ss_flags = SS_DISABLE;
        ::sigaltstack(&disable, nullptr);
        g_ownsAltStack = false;
    }
}

u64 currentPid() {
    return static_cast<u64>(::getpid());
}

#elif defined(FUSE_CRASH_WIN32)

// Win32: a top-level unhandled-exception filter (SEH) plus a SIGABRT handler (abort() never raises
// an SEH exception). Both hand the crash to a dedicated reporter thread created at install time,
// which writes <dir>/fuse_crash_<pid>.dmp (dbghelp MiniDumpWriteDump, resolved at install time)
// and <dir>/fuse_crash_<pid>.txt while the crashing thread waits. Writing from a separate thread
// keeps a usable stack for EXCEPTION_STACK_OVERFLOW and lets dbghelp record the faulting thread
// like any other suspended thread; the exception stream carries its fault context. The filter then
// chains to the previous filter / returns EXCEPTION_CONTINUE_SEARCH, so the process still dies
// with the original exception code (WER / exit status unchanged).

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);
using SignalHandlerFn = void (*)(int);

/// Synthetic exception code recorded for abort() (customer bit set; "FUS" in the low bytes).
constexpr DWORD kAbortExceptionCode = 0xE0465553u;
constexpr DWORD kReporterWaitMs = 60000u;
constexpr usize kMaxModules = 256u;

LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;
MINIDUMP_TYPE g_dumpType = MiniDumpNormal;
SignalHandlerFn g_previousAbort = SIG_DFL;
bool g_abortHooked = false;
std::atomic<int> g_handling{0};

struct CrashRequest {
    EXCEPTION_POINTERS* exception = nullptr;
    DWORD threadId = 0;
    const char* reason = nullptr;
};

CrashRequest g_request;
HANDLE g_reporterThread = nullptr;
DWORD g_reporterThreadId = 0;
HANDLE g_requestEvent = nullptr;
HANDLE g_doneEvent = nullptr;
std::atomic<bool> g_reporterExit{false};

void writeAllWin(HANDLE file, const char* data, usize len) {
    while (len > 0u) {
        DWORD written = 0;
        if (::WriteFile(file, data, static_cast<DWORD>(len), &written, nullptr) == FALSE || written == 0u) {
            return;
        }
        data += written;
        len -= written;
    }
}

void writeStrWin(HANDLE file, const char* s) {
    writeAllWin(file, s, std::strlen(s));
}

const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case kAbortExceptionCode: return "abort (SIGABRT)";
    default: return "exception";
    }
}

/// Unwinds from the exception context (not from the reporter's own stack): frame 0 is the faulting
/// instruction. x64 uses the PE unwind tables (.pdata/.xdata), which MinGW-w64 GCC and MSVC both
/// emit; other architectures fall back to the calling thread's stack.
usize walkStack(const CONTEXT* start, u64* frames, usize capacity) {
#if defined(_M_X64) || defined(__x86_64__)
    if (start == nullptr) {
        return 0u;
    }
    CONTEXT ctx = *start;
    usize count = 0u;
    while (count < capacity && ctx.Rip != 0u) {
        frames[count++] = ctx.Rip;
        const DWORD64 previousSp = ctx.Rsp;
        DWORD64 imageBase = 0u;
        PRUNTIME_FUNCTION function = ::RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (function == nullptr) {
            // Leaf function (no unwind data): the return address is at [rsp].
            if (ctx.Rsp == 0u || (ctx.Rsp & 7u) != 0u) {
                break;
            }
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8u;
        } else {
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0u;
            ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, function, &ctx, &handlerData,
                               &establisherFrame, nullptr);
        }
        if (ctx.Rsp <= previousSp) {
            break; // no progress: corrupt stack or end of chain
        }
    }
    return count;
#else
    (void)start;
    void* raw[kMaxFrames];
    const usize limit = capacity < static_cast<usize>(kMaxFrames) ? capacity : static_cast<usize>(kMaxFrames);
    const USHORT n = ::CaptureStackBackTrace(0, static_cast<DWORD>(limit), raw, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        frames[i] = reinterpret_cast<u64>(raw[i]);
    }
    return n;
#endif
}

void writeMinidump(const CrashRequest& request, const char* path, char* status, usize statusCap) {
    Buf s{status, statusCap, 0u};
    if (g_miniDumpWriteDump == nullptr) {
        s.put("none (dbghelp MiniDumpWriteDump unavailable)");
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = request.threadId;
    mei.ExceptionPointers = request.exception;
    mei.ClientPointers = FALSE;
    // Fallback exception pointers: a copy of the OS-built record and context in 16-byte-aligned
    // static storage with the extended-state (XSTATE) bit cleared. On some CI runners dbghelp fails
    // the abort dump with 0x800706F8 (ERROR_INVALID_USER_BUFFER) on every attempt within a process,
    // while other runners never do. That points at the CPU-dependent XSAVE area appended to an XSTATE
    // context. The copy keeps the integer, control and legacy FP/SSE state a debugger needs.
    alignas(16) static CONTEXT s_contextCopy;
    static EXCEPTION_RECORD s_recordCopy;
    static EXCEPTION_POINTERS s_pointersCopy;
    MINIDUMP_EXCEPTION_INFORMATION meiCopy = mei;
    const bool canSanitize = request.exception != nullptr && request.exception->ExceptionRecord != nullptr &&
                             request.exception->ContextRecord != nullptr;
    if (canSanitize) {
        std::memcpy(&s_recordCopy, request.exception->ExceptionRecord, sizeof(EXCEPTION_RECORD));
        s_recordCopy.ExceptionRecord = nullptr;
        std::memcpy(&s_contextCopy, request.exception->ContextRecord, sizeof(CONTEXT));
#if defined(CONTEXT_XSTATE) && (defined(_M_X64) || defined(__x86_64__))
        s_contextCopy.ContextFlags &= ~(CONTEXT_XSTATE & ~CONTEXT_AMD64);
#endif
        s_pointersCopy.ExceptionRecord = &s_recordCopy;
        s_pointersCopy.ContextRecord = &s_contextCopy;
        meiCopy.ExceptionPointers = &s_pointersCopy;
    }
    // Attempt 1 uses the OS pointers as they are, attempts 2 and 3 the sanitized copy (or the OS
    // pointers again if there is nothing to copy), each into a freshly truncated file.
    constexpr int kDumpAttempts = 3;
    DWORD error = 0u;
    for (int attempt = 1; attempt <= kDumpAttempts; ++attempt) {
        HANDLE dump = ::CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump == INVALID_HANDLE_VALUE) {
            s.put("none (cannot create ");
            s.put(path);
            s.put(")");
            return;
        }
        MINIDUMP_EXCEPTION_INFORMATION* info = nullptr;
        if (request.exception != nullptr) {
            info = (attempt > 1 && canSanitize) ? &meiCopy : &mei;
        }
        const BOOL ok = g_miniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), dump, g_dumpType,
                                            info, nullptr, nullptr);
        error = ok ? 0u : ::GetLastError();
        ::FlushFileBuffers(dump);
        ::CloseHandle(dump);
        if (ok) {
            s.put(path);
            return;
        }
        if (attempt < kDumpAttempts) {
            ::Sleep(10u);
        }
    }
    s.put("failed (MiniDumpWriteDump error ");
    s.putHex(error);
    s.put(")");
}

void writeTextReport(const CrashRequest& request, const char* path, const char* dumpStatus) {
    HANDLE text = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (text == INVALID_HANDLE_VALUE) {
        return;
    }
    const EXCEPTION_RECORD* record =
        request.exception != nullptr ? request.exception->ExceptionRecord : nullptr;
    const CONTEXT* context = request.exception != nullptr ? request.exception->ContextRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0u;

    char line[512];
    Buf b{line, sizeof(line), 0u};
    writeStrWin(text, "FUSE crash report\n");
    b.put("exception: ");
    b.putHex(code);
    b.put(" ");
    b.put(exceptionName(code));
    b.put("\nfault_address: ");
    if (record != nullptr && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2u) {
        b.putHex(static_cast<u64>(record->ExceptionInformation[1]));
        b.put("\naccess: ");
        const ULONG_PTR kind = record->ExceptionInformation[0];
        b.put(kind == 0u ? "read" : (kind == 1u ? "write" : (kind == 8u ? "execute" : "unknown")));
    } else {
        b.put("none");
    }
    b.put("\npc: ");
    b.putHex(record != nullptr ? reinterpret_cast<u64>(record->ExceptionAddress) : 0u);
    b.put("\npid: ");
    b.putDec(static_cast<u64>(::GetCurrentProcessId()));
    b.put("\ntid: ");
    b.putDec(static_cast<u64>(request.threadId));
    b.put("\n");
    writeAllWin(text, line, b.len);

    writeStrWin(text, "dump: ");
    writeStrWin(text, dumpStatus);
    writeStrWin(text, "\nnote: ");
    writeAllWin(text, g_note, ::strnlen(g_note, sizeof(g_note)));
    writeStrWin(text, "\nframes:\n");
    u64 frames[kMaxFrames];
    const usize count = walkStack(context, frames, static_cast<usize>(kMaxFrames));
    for (usize i = 0; i < count; ++i) {
        b.len = 0u;
        b.put("  ");
        b.putHex(frames[i]);
        b.put("\n");
        writeAllWin(text, line, b.len);
    }

    // Module map (base, size, path): symbolise the frames offline (addr2line / llvm-symbolizer on
    // image-relative addresses, or WinDbg with the .dmp).
    writeStrWin(text, "modules:\n");
    HMODULE modules[kMaxModules];
    DWORD needed = 0;
    if (::K32EnumProcessModules(::GetCurrentProcess(), modules, sizeof(modules), &needed) != FALSE) {
        usize n = needed / sizeof(HMODULE);
        n = n < kMaxModules ? n : kMaxModules;
        for (usize i = 0; i < n; ++i) {
            MODULEINFO info{};
            if (::K32GetModuleInformation(::GetCurrentProcess(), modules[i], &info, sizeof(info)) == FALSE) {
                continue;
            }
            b.len = 0u;
            b.put("  ");
            b.putHex(reinterpret_cast<u64>(info.lpBaseOfDll));
            b.put(" ");
            b.putHex(static_cast<u64>(info.SizeOfImage));
            b.put(" ");
            char name[MAX_PATH];
            const DWORD nameLen = ::GetModuleFileNameA(modules[i], name, MAX_PATH);
            name[nameLen < MAX_PATH ? nameLen : MAX_PATH - 1] = '\0';
            b.put(name);
            b.put("\n");
            writeAllWin(text, line, b.len);
        }
    }
    writeStrWin(text, "end\n");
    ::FlushFileBuffers(text);
    ::CloseHandle(text);
}

void writeCrashArtifacts(const CrashRequest& request) {
    const u64 pid = static_cast<u64>(::GetCurrentProcessId());
    char path[kPathCapacity + 64];
    Buf p{path, sizeof(path), 0u};
    buildReportPath(p, pid, ".dmp");
    char dumpStatus[kPathCapacity + 128];
    dumpStatus[0] = '\0';
    writeMinidump(request, path, dumpStatus, sizeof(dumpStatus));

    p.len = 0u;
    buildReportPath(p, pid, ".txt");
    writeTextReport(request, path, dumpStatus);

    char msg[kPathCapacity + 160];
    Buf m{msg, sizeof(msg), 0u};
    m.put("FUSE: fatal ");
    m.put(request.reason != nullptr ? request.reason : "exception");
    m.put(", crash report written to ");
    m.put(path);
    m.put("\n");
    HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (err != nullptr && err != INVALID_HANDLE_VALUE) {
        writeAllWin(err, msg, m.len);
    }
}

DWORD WINAPI crashReporterMain(LPVOID) {
    for (;;) {
        ::WaitForSingleObject(g_requestEvent, INFINITE);
        if (g_reporterExit.load(std::memory_order_acquire)) {
            return 0;
        }
        writeCrashArtifacts(g_request);
        ::SetEvent(g_doneEvent);
    }
}

/// Returns false when another crash is already being reported (nested fault or a second thread).
bool reportCrash(EXCEPTION_POINTERS* ep, const char* reason) {
    int expected = 0;
    if (!g_handling.compare_exchange_strong(expected, 1)) {
        return false;
    }
    g_request.exception = ep;
    g_request.threadId = ::GetCurrentThreadId();
    g_request.reason = reason;
    if (g_reporterThread != nullptr && g_request.threadId != g_reporterThreadId) {
        ::SetEvent(g_requestEvent);
        ::WaitForSingleObject(g_doneEvent, kReporterWaitMs);
    } else {
        writeCrashArtifacts(g_request);
    }
    return true;
}

LONG WINAPI crashExceptionFilter(EXCEPTION_POINTERS* ep) {
    const DWORD code = (ep != nullptr && ep->ExceptionRecord != nullptr) ? ep->ExceptionRecord->ExceptionCode : 0u;
    if (!reportCrash(ep, exceptionName(code))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_previousFilter != nullptr) {
        return g_previousFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// Vectored handler installed only while crashAbortHandler raises the synthetic abort exception:
/// it reports with the EXCEPTION_POINTERS the OS built for RaiseException and resumes.
LONG CALLBACK crashAbortVectoredHandler(EXCEPTION_POINTERS* ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr || ep->ExceptionRecord->ExceptionCode != kAbortExceptionCode) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    reportCrash(ep, "abort (SIGABRT)");
    return EXCEPTION_CONTINUE_EXECUTION;
}

void crashAbortHandler(int sig) {
    // abort() raises no SEH exception. Raise a continuable synthetic one and report it from a
    // first-chance vectored handler: MiniDumpWriteDump then gets OS-built exception pointers.
    // (A hand-made EXCEPTION_RECORD + RtlCaptureContext context made dbghelp fail with
    // RPC_X_NULL_REF_POINTER under MSVC builds and drop the ExceptionStream.)
    PVOID vectored = ::AddVectoredExceptionHandler(1u, &crashAbortVectoredHandler);
    if (vectored != nullptr) {
        ::RaiseException(kAbortExceptionCode, 0u, 0u, nullptr);
        ::RemoveVectoredExceptionHandler(vectored);
    } else {
        CONTEXT context{};
        ::RtlCaptureContext(&context);
        EXCEPTION_RECORD record{};
        record.ExceptionCode = kAbortExceptionCode;
        record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64) || defined(__x86_64__)
        record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
#endif
        EXCEPTION_POINTERS pointers{&record, &context};
        reportCrash(&pointers, "abort (SIGABRT)");
    }

    // Chain: restore the previous disposition. With SIG_DFL, returning lets abort() terminate the
    // process (exit code 3); a previously installed handler is invoked directly.
    ::signal(SIGABRT, g_previousAbort);
    if (g_previousAbort != SIG_DFL && g_previousAbort != SIG_IGN && g_previousAbort != SIG_ERR &&
        g_previousAbort != nullptr) {
        g_previousAbort(sig);
    }
}

template <typename Fn>
Fn resolveProc(HMODULE module, const char* name) {
    // Via a generic function pointer: FARPROC -> specific signature without -Wcast-function-type.
    return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(::GetProcAddress(module, name)));
}

HMODULE loadSystemLibrary(const char* name) {
    // System32 only (no DLL planting from the working directory); plain search on systems without
    // LOAD_LIBRARY_SEARCH_* support (pre-KB2533623 Windows 7).
    HMODULE module = ::LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        module = ::LoadLibraryA(name);
    }
    return module;
}

bool installOs() {
    if (g_miniDumpWriteDump == nullptr) {
        HMODULE dbghelp = loadSystemLibrary("dbghelp.dll");
        if (dbghelp != nullptr) {
            g_miniDumpWriteDump = resolveProc<MiniDumpWriteDumpFn>(dbghelp, "MiniDumpWriteDump");
        }
    }
    // Default: stacks, thread info, module data segments and memory referenced from the stacks
    // (a few MB, enough for WinDbg `!analyze -v`, `.ecxr`, `k`). FUSE_CRASH_DUMP=full adds the whole
    // address space (heap included) for post-mortem debugging of memory corruption.
    int type = MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
               MiniDumpWithUnloadedModules;
    const char* mode = std::getenv("FUSE_CRASH_DUMP");
    if (mode != nullptr && std::strcmp(mode, "full") == 0) {
        type |= MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithFullMemoryInfo;
    }
    g_dumpType = static_cast<MINIDUMP_TYPE>(type);

    // Guarantee stack for the filter itself on the installing (main) thread after an overflow.
    ULONG guarantee = 32u * 1024u;
    ::SetThreadStackGuarantee(&guarantee);

    if (g_reporterThread == nullptr) {
        g_reporterExit.store(false, std::memory_order_release);
        g_requestEvent = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_doneEvent = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (g_requestEvent != nullptr && g_doneEvent != nullptr) {
            g_reporterThread = ::CreateThread(nullptr, 256u * 1024u, &crashReporterMain, nullptr,
                                              STACK_SIZE_PARAM_IS_A_RESERVATION, &g_reporterThreadId);
        }
    }

    g_previousFilter = ::SetUnhandledExceptionFilter(&crashExceptionFilter);
    const SignalHandlerFn previousAbort = ::signal(SIGABRT, &crashAbortHandler);
    g_abortHooked = previousAbort != SIG_ERR;
    g_previousAbort = g_abortHooked ? previousAbort : SIG_DFL;
    g_handling.store(0);
    return true;
}

void shutdownOs() {
    ::SetUnhandledExceptionFilter(g_previousFilter);
    g_previousFilter = nullptr;
    if (g_abortHooked) {
        ::signal(SIGABRT, g_previousAbort);
        g_abortHooked = false;
    }
    if (g_reporterThread != nullptr) {
        g_reporterExit.store(true, std::memory_order_release);
        ::SetEvent(g_requestEvent);
        ::WaitForSingleObject(g_reporterThread, INFINITE);
        ::CloseHandle(g_reporterThread);
        g_reporterThread = nullptr;
        g_reporterThreadId = 0;
    }
    if (g_requestEvent != nullptr) {
        ::CloseHandle(g_requestEvent);
        g_requestEvent = nullptr;
    }
    if (g_doneEvent != nullptr) {
        ::CloseHandle(g_doneEvent);
        g_doneEvent = nullptr;
    }
}

u64 currentPid() {
    return static_cast<u64>(::GetCurrentProcessId());
}
#else

bool installOs() {
    return false;
}

void shutdownOs() {}

u64 currentPid() {
    return 0u;
}

#endif

} // namespace

bool installCrashHandlers() {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    resolveDefaultDirectory();
    const bool ok = installOs();
    g_installed.store(ok, std::memory_order_release);
    return ok;
}

void shutdownCrashHandlers() {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    shutdownOs();
}

bool crashHandlersInstalled() {
    return g_installed.load(std::memory_order_acquire);
}

void setCrashReportDirectory(const char* directory) {
    Buf b{g_directory, sizeof(g_directory), 0u};
    g_directory[0] = '\0';
    b.put((directory != nullptr && directory[0] != '\0') ? directory : ".");
    g_directorySet = true;
}

std::string crashReportPath() {
    resolveDefaultDirectory();
    char path[kPathCapacity + 64];
    Buf p{path, sizeof(path), 0u};
    path[0] = '\0';
    buildReportPath(p, currentPid(), ".txt");
    return std::string(path, p.len);
}

void setCrashContextNote(const char* note) {
    // Clear first so a crash racing this copy never sees an unterminated buffer.
    g_note[0] = '\0';
    if (note == nullptr) {
        return;
    }
    const usize n = ::strnlen(note, kNoteCapacity - 1u);
    std::memcpy(g_note + 1, note + 1, n > 0u ? n - 1u : 0u);
    g_note[n] = '\0';
    if (n > 0u) {
        g_note[0] = note[0];
    }
}

} // namespace fuse::platform
