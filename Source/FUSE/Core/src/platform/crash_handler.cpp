// Process crash handlers (B7.8 platform hardening).
//
// Everything reachable from the signal handler / exception filter is async-signal-safe: fixed
// static buffers, open/write/close, no heap, no locks, no stdio. Configuration (directory, note)
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
#include <windows.h>
#include <dbghelp.h>
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

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;
std::atomic<int> g_handling{0};

void writeAllWin(HANDLE file, const char* data, usize len) {
    DWORD written = 0;
    ::WriteFile(file, data, static_cast<DWORD>(len), &written, nullptr);
}

LONG WINAPI crashExceptionFilter(EXCEPTION_POINTERS* ep) {
    int expected = 0;
    if (!g_handling.compare_exchange_strong(expected, 1)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 pid = static_cast<u64>(::GetCurrentProcessId());

    char path[kPathCapacity + 64];
    Buf p{path, sizeof(path), 0u};
    buildReportPath(p, pid, ".dmp");
    if (g_miniDumpWriteDump != nullptr) {
        HANDLE dump = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = ::GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
            g_miniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), dump, type, &mei, nullptr,
                                nullptr);
            ::CloseHandle(dump);
        }
    }

    p.len = 0u;
    buildReportPath(p, pid, ".txt");
    HANDLE text = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (text != INVALID_HANDLE_VALUE) {
        char line[512];
        Buf b{line, sizeof(line), 0u};
        b.put("FUSE crash report\nexception: ");
        b.putHex(ep != nullptr && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0u);
        b.put("\npc: ");
        b.putHex(ep != nullptr && ep->ExceptionRecord
                     ? reinterpret_cast<u64>(ep->ExceptionRecord->ExceptionAddress)
                     : 0u);
        b.put("\npid: ");
        b.putDec(pid);
        b.put("\nnote: ");
        writeAllWin(text, line, b.len);
        writeAllWin(text, g_note, ::strnlen(g_note, sizeof(g_note)));
        writeAllWin(text, "\nframes:\n", 9u);
        void* frames[kMaxFrames];
        const USHORT count = ::CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
        for (USHORT i = 0; i < count; ++i) {
            b.len = 0u;
            b.put("  ");
            b.putHex(reinterpret_cast<u64>(frames[i]));
            b.put("\n");
            writeAllWin(text, line, b.len);
        }
        writeAllWin(text, "end\n", 4u);
        ::CloseHandle(text);
    }

    if (g_previousFilter != nullptr) {
        return g_previousFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool installOs() {
    if (g_miniDumpWriteDump == nullptr) {
        HMODULE dbghelp = ::LoadLibraryA("dbghelp.dll");
        if (dbghelp != nullptr) {
            g_miniDumpWriteDump =
                reinterpret_cast<MiniDumpWriteDumpFn>(::GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        }
    }
    g_previousFilter = ::SetUnhandledExceptionFilter(&crashExceptionFilter);
    g_handling.store(0);
    return true;
}

void shutdownOs() {
    ::SetUnhandledExceptionFilter(g_previousFilter);
    g_previousFilter = nullptr;
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
