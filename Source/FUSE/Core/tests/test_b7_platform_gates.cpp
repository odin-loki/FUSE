// B7.8 platform hardening gates (FUSE_MASTER_PLAN "B7.8 — Platform Hardening" and the platform rows
// of "B7.10 — Phase 7 Deliverables & Test Suite"):
//   - Crash handler writes a valid crash report on an intentional null dereference (Linux analogue
//     of the WinDbg minidump row): the child process still dies with SIGSEGV, the report names the
//     signal and fault address, its faulting pc lies inside the crashing function according to the
//     ELF symbol table, the in-report symbols name that function, and addr2line resolves the
//     reported pc offline through the report's module map. Also: failed FUSE_VERIFY (SIGABRT, note
//     carries message/file/line), integer divide by zero (SIGFPE), stack overflow (alternate
//     signal stack), a crash on a worker thread, chaining to a previously installed handler, and
//     shutdown restoring it.
//   - Leak detector reports zero leaks after a clean engine shutdown in a debug build, and reports
//     exactly the deliberately leaked allocations otherwise. The table is checked against an
//     independent ledger under random churn with forced hash collisions, and an init/shutdown soak
//     is checked against this binary's own global operator new/delete live-block counter.
// Windows: the minidump + SEH path is gated by fuse_core_b7_win32_crash_minidump and DPI awareness by
// fuse_core_b7_dpi_awareness (MinGW cross build under Wine); opening the .dmp in WinDbg and the full
// shipping binary inspection on MSVC stay manual.

#include <fuse/alloc/freelist_allocator.hpp>
#include <fuse/alloc/leak_detector.hpp>
#include <fuse/alloc/new_ban.hpp>
#include <fuse/alloc/pool_allocator.hpp>
#include <fuse/assert.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/crash_report.hpp>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/wait.h>
#include <unistd.h>

#define FUSE_B7_LINUX 1
#endif

// AddressSanitizer owns SIGSEGV/SIGFPE (it ignores user sigaction for them unless told otherwise),
// so the fault-signal probes run in the plain Debug/Release builds; sanitizer builds keep the
// SIGABRT, leak and ownership gates.
#if defined(__SANITIZE_ADDRESS__)
#define FUSE_B7_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FUSE_B7_ASAN 1
#endif
#endif

// The probes crash on purpose; keep UBSan from reporting (and halting on) the deliberate UB.
#define FUSE_B7_CRASH_PROBE extern "C" __attribute__((noinline, visibility("default"), no_sanitize("undefined")))

#if defined(FUSE_B7_LINUX)

// ---- global heap live-block counter (whole binary, independent of the leak detector) ------------

namespace {
std::atomic<long long> g_liveHeapBlocks{0};
} // namespace

// Replacement allocation/deallocation functions stay out of line: once GCC inlines one of them into
// a std::allocator call site it pairs its malloc()/free() with the other side's builtin
// ::operator new/delete and reports a false -Wmismatched-new-delete (replacement functions must not
// be inline anyway, [replacement.functions]).
#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    g_liveHeapBlocks.fetch_add(1, std::memory_order_relaxed);
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) {
    return ::operator new(size);
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p != nullptr) {
        g_liveHeapBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size, std::align_val_t alignment) {
    const std::size_t a = static_cast<std::size_t>(alignment);
    void* p = std::aligned_alloc(a, ((size == 0 ? 1 : size) + a - 1) / a * a);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    g_liveHeapBlocks.fetch_add(1, std::memory_order_relaxed);
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept {
    if (p != nullptr) {
        g_liveHeapBlocks.fetch_sub(1, std::memory_order_relaxed);
        std::free(p);
    }
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept {
    ::operator delete(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept {
    ::operator delete(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept {
    ::operator delete(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::align_val_t) noexcept {
    ::operator delete(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::align_val_t) noexcept {
    ::operator delete(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    ::operator delete(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    ::operator delete(p);
}

// ---- crash probes (exported, never inlined: the report must attribute the fault to them) --------

int* volatile g_b7NullTarget = nullptr;
volatile int g_b7Zero = 0;
volatile int g_b7RecurseLimit = 0;

FUSE_B7_CRASH_PROBE void fuse_b7_crash_probe_null_deref() {
    *g_b7NullTarget = 42;
}

FUSE_B7_CRASH_PROBE int fuse_b7_crash_probe_div_zero(int value) {
    return value / g_b7Zero;
}

FUSE_B7_CRASH_PROBE int fuse_b7_crash_probe_recurse(int depth) {
    volatile char pad[512];
    pad[0] = static_cast<char>(depth);
    if (g_b7RecurseLimit != 0 && depth > g_b7RecurseLimit) {
        return pad[0];
    }
    return fuse_b7_crash_probe_recurse(depth + 1) + pad[0];
}

#endif // FUSE_B7_LINUX

namespace {

using fuse::u32;
using fuse::u64;
using fuse::usize;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void quietSink(fuse::log::Level, const char*, void*) {}

std::filesystem::path g_tempDir;

#if defined(FUSE_B7_LINUX)

// ---- crash report helpers -----------------------------------------------------------------------

struct ChildResult {
    pid_t pid = -1;
    int status = 0;
};

template <typename Fn>
ChildResult runChild(Fn&& body) {
    std::fflush(nullptr);
    ChildResult result;
    result.pid = fork();
    if (result.pid == 0) {
        fuse::log::Logger::instance().setSink(quietSink, nullptr);
        fuse::platform::setCrashReportDirectory(g_tempDir.c_str());
        body();
        _exit(0);
    }
    waitpid(result.pid, &result.status, 0);
    return result;
}

std::string reportPathFor(pid_t pid) {
    return (g_tempDir / ("fuse_crash_" + std::to_string(pid) + ".txt")).string();
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string field(const std::string& report, const char* name) {
    const std::string key = std::string("\n") + name + ": ";
    const usize at = report.find(key);
    if (at == std::string::npos) {
        return {};
    }
    const usize begin = at + key.size();
    const usize end = report.find('\n', begin);
    return report.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::string section(const std::string& report, const char* name, const char* next) {
    const usize begin = report.find(std::string("\n") + name + ":\n");
    // The report ends with a bare "end" line; every other section header carries a colon.
    const usize end = std::strcmp(next, "end") == 0 ? report.find("\nend\n")
                                                    : report.find(std::string("\n") + next + ":\n");
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
        return {};
    }
    return report.substr(begin, end - begin);
}

u64 parseHex(const std::string& text) {
    return text.empty() ? 0u : std::strtoull(text.c_str(), nullptr, 16);
}

/// [start, start + size) of an exported function, from the ELF dynamic symbol table.
bool symbolRange(const void* fn, uintptr_t& outStart, uintptr_t& outEnd) {
    Dl_info info{};
    void* extra = nullptr;
    const int found = dladdr1(fn, &info, &extra, RTLD_DL_SYMENT);
    const auto* sym = static_cast<const ElfW(Sym)*>(extra);
    if (found == 0 || sym == nullptr ||
        info.dli_saddr == nullptr || sym->st_size == 0) {
        return false;
    }
    outStart = reinterpret_cast<uintptr_t>(info.dli_saddr);
    outEnd = outStart + sym->st_size;
    return true;
}

std::vector<u64> frameAddresses(const std::string& report) {
    std::vector<u64> frames;
    std::istringstream in(section(report, "frames", "symbols"));
    std::string line;
    while (std::getline(in, line)) {
        const usize at = line.find("0x");
        if (at != std::string::npos) {
            frames.push_back(parseHex(line.substr(at)));
        }
    }
    return frames;
}

/// Offline symbolisation through the report's module map: find the mapping holding `pc`, turn it
/// into a module-relative address (PIE) and ask addr2line for the function name.
std::string symboliseOffline(const std::string& report, u64 pc) {
    std::istringstream in(section(report, "maps", "end") + "\n");
    std::string line;
    std::string modulePath;
    std::map<std::string, u64> moduleBase;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    for (const std::string& l : lines) {
        unsigned long long lo = 0, hi = 0, offset = 0;
        char perms[8] = {};
        char path[1024] = {};
        if (std::sscanf(l.c_str(), "%llx-%llx %7s %llx %*s %*s %1023s", &lo, &hi, perms, &offset, path) >= 4 &&
            path[0] == '/') {
            if (moduleBase.find(path) == moduleBase.end()) {
                moduleBase[path] = lo - offset; // first mapping of a module (file offset 0)
            }
            if (pc >= lo && pc < hi) {
                modulePath = path;
            }
        }
    }
    if (modulePath.empty()) {
        return {};
    }
    // ET_DYN (PIE / shared object) needs the module-relative address, ET_EXEC the absolute one.
    Elf64_Ehdr header{};
    std::ifstream elf(modulePath, std::ios::binary);
    elf.read(reinterpret_cast<char*>(&header), sizeof(header));
    const u64 address = header.e_type == ET_DYN ? pc - moduleBase[modulePath] : pc;
    char command[1400];
    std::snprintf(command, sizeof(command), "addr2line -f -e '%s' 0x%" PRIx64 " 2>/dev/null", modulePath.c_str(),
                  address);
    FILE* pipe = popen(command, "r");
    if (pipe == nullptr) {
        return {};
    }
    char name[512] = {};
    const bool got = std::fgets(name, sizeof(name), pipe) != nullptr;
    pclose(pipe);
    if (!got) {
        return {};
    }
    std::string result(name);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

bool haveAddr2line() {
    return std::system("command -v addr2line >/dev/null 2>&1") == 0;
}

// ---- crash handler gates ------------------------------------------------------------------------

void testNullDereferenceReport() {
    const ChildResult child = runChild([] {
        fuse::platform::installCrashHandlers();
        fuse_b7_crash_probe_null_deref();
    });
    expectTrue(WIFSIGNALED(child.status) && WTERMSIG(child.status) == SIGSEGV,
               "null dereference: process still terminates with SIGSEGV after the report");

    const std::string report = readFile(reportPathFor(child.pid));
    expectTrue(report.rfind("FUSE crash report\n", 0) == 0, "null dereference: report written");
    expectTrue(report.size() > 16 && report.compare(report.size() - 4, 4, "end\n") == 0,
               "null dereference: report complete (terminator present)");
    expectTrue(field(report, "signal") == "11 SIGSEGV", "null dereference: signal recorded as 11 SIGSEGV");
    expectTrue(field(report, "code") == std::to_string(SEGV_MAPERR), "null dereference: si_code SEGV_MAPERR");
    expectTrue(field(report, "fault_address") == "0x0000000000000000", "null dereference: fault address 0x0");
    expectTrue(field(report, "pid") == std::to_string(child.pid), "null dereference: pid recorded");

    uintptr_t fnStart = 0;
    uintptr_t fnEnd = 0;
    const bool haveRange =
        symbolRange(reinterpret_cast<const void*>(&fuse_b7_crash_probe_null_deref), fnStart, fnEnd);
    expectTrue(haveRange, "null dereference: probe symbol has an ELF size (ENABLE_EXPORTS)");
    const u64 pc = parseHex(field(report, "pc"));
    bool pcInFunction = pc >= fnStart && pc < fnEnd;
#if !defined(__x86_64__) && !defined(__aarch64__)
    pcInFunction = true; // no ucontext pc on this architecture
#endif
    expectTrue(pcInFunction, "null dereference: faulting pc lies inside fuse_b7_crash_probe_null_deref");

    u32 framesInFunction = 0;
    const std::vector<u64> frames = frameAddresses(report);
    for (u64 frame : frames) {
        framesInFunction += (frame >= fnStart && frame < fnEnd) ? 1u : 0u;
    }
    expectTrue(frames.size() >= 3u && framesInFunction >= 1u,
               "null dereference: stack frames include the crashing function");
    expectTrue(section(report, "symbols", "maps").find("fuse_b7_crash_probe_null_deref") != std::string::npos,
               "null dereference: symbolised stack names the crashing function");
    expectTrue(section(report, "symbols", "maps").find("main") != std::string::npos,
               "null dereference: symbolised stack reaches main");

    if (haveAddr2line()) {
        const std::string offline = symboliseOffline(report, pc);
        std::printf("  null deref: pc %#" PRIx64 " (fn %#" PRIxPTR "+%" PRIuPTR "), addr2line -> %s\n", pc,
                    fnStart, static_cast<uintptr_t>(pc - fnStart), offline.c_str());
        expectTrue(offline == "fuse_b7_crash_probe_null_deref",
                   "null dereference: addr2line resolves the reported pc via the module map");
    } else {
        std::printf("  null deref: addr2line not installed, offline symbolisation not checked\n");
    }
    std::printf("  null deref: report %zu bytes, %zu frames\n", report.size(), frames.size());
}

#if !(defined(FUSE_NO_ASSERT) && FUSE_NO_ASSERT)
constexpr int kVerifyProbeLine = __LINE__ + 2;
void verifyProbe() {
    FUSE_VERIFY(g_b7Zero == 1, "b7 crash probe verify");
}
#endif

void testAssertAbortReport() {
#if !(defined(FUSE_NO_ASSERT) && FUSE_NO_ASSERT)
    const ChildResult child = runChild([] {
        fuse::assertion::setSuppressAbortForTests(false);
        fuse::platform::installCrashHandlers();
        verifyProbe();
    });
    const int line = kVerifyProbeLine;
    expectTrue(WIFSIGNALED(child.status) && WTERMSIG(child.status) == SIGABRT,
               "failed FUSE_VERIFY: process terminates with SIGABRT");
    const std::string report = readFile(reportPathFor(child.pid));
    expectTrue(field(report, "signal") == "6 SIGABRT", "failed FUSE_VERIFY: signal recorded as 6 SIGABRT");
    expectTrue(field(report, "fault_address").rfind("none", 0) == 0,
               "failed FUSE_VERIFY: sent signal reports no fault address");
    const std::string expectedNote =
        std::string("fatal: b7 crash probe verify (") + __FILE__ + ":" + std::to_string(line) + ")";
    expectTrue(field(report, "note") == expectedNote, "failed FUSE_VERIFY: note carries message, file and line");
    // fatal() ends in a noreturn call, so its return address can sit past the function's last byte;
    // symbolise return addresses minus one, as a debugger does.
    bool fatalFrame = false;
    if (haveAddr2line()) {
        for (u64 frame : frameAddresses(report)) {
            fatalFrame = fatalFrame || symboliseOffline(report, frame - 1u).find("assertion5fatal") != std::string::npos;
        }
    } else {
        fatalFrame = section(report, "symbols", "maps").find("fatal") != std::string::npos;
    }
    expectTrue(fatalFrame, "failed FUSE_VERIFY: stack includes fuse::assertion::fatal");
#endif
}

void testDivideByZeroReport() {
#if defined(__x86_64__) || defined(__i386__)
    const ChildResult child = runChild([] {
        fuse::platform::installCrashHandlers();
        std::printf("%d\n", fuse_b7_crash_probe_div_zero(7));
    });
    expectTrue(WIFSIGNALED(child.status) && WTERMSIG(child.status) == SIGFPE,
               "divide by zero: process terminates with SIGFPE");
    const std::string report = readFile(reportPathFor(child.pid));
    expectTrue(field(report, "signal") == "8 SIGFPE", "divide by zero: signal recorded as 8 SIGFPE");
    expectTrue(field(report, "code") == std::to_string(FPE_INTDIV), "divide by zero: si_code FPE_INTDIV");
    uintptr_t fnStart = 0;
    uintptr_t fnEnd = 0;
    symbolRange(reinterpret_cast<const void*>(&fuse_b7_crash_probe_div_zero), fnStart, fnEnd);
    const u64 pc = parseHex(field(report, "pc"));
    expectTrue(pc >= fnStart && pc < fnEnd, "divide by zero: faulting pc inside fuse_b7_crash_probe_div_zero");
#endif
}

void testStackOverflowReport() {
    const ChildResult child = runChild([] {
        fuse::platform::installCrashHandlers();
        std::printf("%d\n", fuse_b7_crash_probe_recurse(0));
    });
    expectTrue(WIFSIGNALED(child.status) && WTERMSIG(child.status) == SIGSEGV,
               "stack overflow: process terminates with SIGSEGV");
    const std::string report = readFile(reportPathFor(child.pid));
    expectTrue(field(report, "signal") == "11 SIGSEGV", "stack overflow: report written from the alternate stack");
    expectTrue(section(report, "symbols", "maps").find("fuse_b7_crash_probe_recurse") != std::string::npos,
               "stack overflow: symbolised stack names the recursing function");
}

void testWorkerThreadCrash() {
    const ChildResult child = runChild([] {
        fuse::platform::installCrashHandlers();
        std::thread worker([] { fuse_b7_crash_probe_null_deref(); });
        worker.join();
    });
    expectTrue(WIFSIGNALED(child.status) && WTERMSIG(child.status) == SIGSEGV,
               "worker thread crash: process terminates with SIGSEGV");
    const std::string report = readFile(reportPathFor(child.pid));
    const std::string tid = field(report, "tid");
    expectTrue(field(report, "signal") == "11 SIGSEGV" && !tid.empty() && tid != std::to_string(child.pid),
               "worker thread crash: report written and names the faulting (non-main) thread");
    expectTrue(section(report, "symbols", "maps").find("fuse_b7_crash_probe_null_deref") != std::string::npos,
               "worker thread crash: symbolised stack names the crashing function");
}

extern "C" void b7SentinelHandler(int, siginfo_t*, void*) {
    _exit(42);
}

void installSentinel() {
    struct sigaction sentinel {};
    sentinel.sa_sigaction = &b7SentinelHandler;
    sentinel.sa_flags = SA_SIGINFO;
    sigemptyset(&sentinel.sa_mask);
    sigaction(SIGSEGV, &sentinel, nullptr);
}

void testChainsToPreviousHandler() {
    const ChildResult child = runChild([] {
        fuse::platform::shutdownCrashHandlers();
        installSentinel();
        fuse::platform::installCrashHandlers();
        fuse_b7_crash_probe_null_deref();
    });
    expectTrue(WIFEXITED(child.status) && WEXITSTATUS(child.status) == 42,
               "chaining: previously installed SIGSEGV handler still runs after the report");
    expectTrue(field(readFile(reportPathFor(child.pid)), "signal") == "11 SIGSEGV",
               "chaining: report written before chaining");
}

void testShutdownRestoresHandlers() {
    const ChildResult child = runChild([] {
        fuse::platform::shutdownCrashHandlers();
        installSentinel();
        fuse::platform::installCrashHandlers();
        fuse::platform::shutdownCrashHandlers();
        struct sigaction current {};
        sigaction(SIGSEGV, nullptr, &current);
        if (current.sa_sigaction != &b7SentinelHandler || fuse::platform::crashHandlersInstalled()) {
            _exit(3);
        }
        fuse_b7_crash_probe_null_deref();
    });
    expectTrue(WIFEXITED(child.status) && WEXITSTATUS(child.status) == 42,
               "shutdown: previous SIGSEGV handler restored");
    expectTrue(!std::filesystem::exists(reportPathFor(child.pid)), "shutdown: no report once handlers are removed");
}

void testReportPathAndNote() {
    fuse::platform::setCrashReportDirectory(g_tempDir.c_str());
    expectTrue(fuse::platform::crashReportPath() == reportPathFor(getpid()),
               "crashReportPath() is <dir>/fuse_crash_<pid>.txt");
    std::string longNote(2000, 'x');
    fuse::platform::setCrashContextNote(longNote.c_str());
    const ChildResult child = runChild([] {
        fuse::platform::installCrashHandlers();
        fuse_b7_crash_probe_null_deref();
    });
    expectTrue(field(readFile(reportPathFor(child.pid)), "note") == std::string(511, 'x'),
               "context note truncated to 511 bytes");
    fuse::platform::setCrashContextNote(nullptr);
}

#endif // FUSE_B7_LINUX

// ---- leak detector ------------------------------------------------------------------------------

void testLeakTableAgainstLedger() {
    using fuse::alloc::LeakDetector;
    LeakDetector::clear();
    std::mt19937_64 rng(0xB78u);
    std::map<uintptr_t, usize> ledger;
    // Pointers packed 16 bytes apart in a few dense clusters: many share a home slot run.
    auto randomPtr = [&]() {
        const uintptr_t cluster = 0x100000u * (1u + rng() % 4u);
        return cluster + 16u * (rng() % 3000u);
    };
    u32 mismatches = 0;
    for (u32 step = 0; step < 60000u; ++step) {
        const u32 op = static_cast<u32>(rng() % 10u);
        if (op < 6u) {
            const uintptr_t p = randomPtr();
            const usize size = 1u + static_cast<usize>(rng() % 4096u);
            LeakDetector::recordAlloc(reinterpret_cast<const void*>(p), size, "ledger");
            ledger[p] = size;
        } else if (op < 9u) {
            const uintptr_t p = randomPtr();
            const bool known = ledger.erase(p) != 0u;
            mismatches += LeakDetector::recordFree(reinterpret_cast<const void*>(p)) != known ? 1u : 0u;
        } else if (rng() % 50u == 0u) {
            const uintptr_t lo = randomPtr();
            const usize bytes = 16u * (1u + rng() % 200u);
            LeakDetector::releaseRange(reinterpret_cast<const void*>(lo), bytes);
            ledger.erase(ledger.lower_bound(lo), ledger.lower_bound(lo + bytes));
        }
        if (step % 997u == 0u) {
            usize bytes = 0;
            for (const auto& [p, s] : ledger) {
                bytes += s;
            }
            mismatches += (LeakDetector::liveCount() != ledger.size() || LeakDetector::liveBytes() != bytes) ? 1u : 0u;
        }
    }
    std::map<uintptr_t, usize> fromTable;
    for (const auto& r : LeakDetector::liveRecords()) {
        fromTable[reinterpret_cast<uintptr_t>(r.ptr)] = r.size;
    }
    std::printf("  leak table: %zu live after 60000 random ops, %u mismatches vs ledger\n", ledger.size(),
                mismatches);
    expectTrue(mismatches == 0u && fromTable == ledger, "leak table matches an independent ledger under churn");
    LeakDetector::clear();
    expectTrue(LeakDetector::liveCount() == 0u, "leak table clears");
}

void testAllocatorHooks() {
    using fuse::alloc::LeakDetector;
    if constexpr (!fuse::alloc::kLeakDetectorEnabled) {
        // Release: allocator hooks are compiled out; the pool leaves the table untouched.
        LeakDetector::clear();
        fuse::alloc::PoolAllocator pool(64u, 8u, "b7.pool");
        void* p = pool.alloc({48u, 16u, nullptr});
        expectTrue(LeakDetector::liveCount() == 0u, "release build: allocator hooks compiled out");
        pool.free(p, 48u);
        return;
    }
    LeakDetector::clear();
    {
        fuse::alloc::PoolAllocator pool(64u, 32u, "b7.pool");
        fuse::alloc::FreeListAllocator heap(8192u, "b7.freelist");
        std::map<const void*, usize> expected;
        std::vector<std::pair<void*, usize>> poolLive;
        std::vector<std::pair<void*, usize>> heapLive;
        for (u32 i = 0; i < 16u; ++i) {
            const usize ps = 8u + i;
            void* p = pool.alloc({ps, 8u, "b7.tag"});
            poolLive.push_back({p, ps});
            expected[p] = ps;
            const usize hs = 24u + 8u * i;
            void* h = heap.alloc({hs, 16u, nullptr});
            heapLive.push_back({h, hs});
            expected[h] = hs;
        }
        for (u32 i = 0; i < 16u; i += 2u) {
            pool.free(poolLive[i].first, poolLive[i].second);
            expected.erase(poolLive[i].first);
            heap.free(heapLive[i].first, heapLive[i].second);
            expected.erase(heapLive[i].first);
        }
        std::map<const void*, usize> tracked;
        bool tagsOk = true;
        for (const auto& r : LeakDetector::liveRecords()) {
            tracked[r.ptr] = r.size;
            const bool isPool = pool.isLive(r.ptr);
            tagsOk = tagsOk && std::strcmp(r.tag, isPool ? "b7.tag" : "b7.freelist") == 0;
        }
        expectTrue(tracked == expected && tagsOk,
                   "debug: pool/free-list hooks track exactly the outstanding blocks with request sizes and tags");

        const fuse::alloc::LeakReport report = LeakDetector::snapshot();
        usize bytes = 0;
        for (const auto& [p, s] : expected) {
            bytes += s;
        }
        expectTrue(report.liveCount == 16u && report.liveBytes == bytes, "debug: report totals match (16 blocks)");

        pool.reset();
        expectTrue(LeakDetector::liveCount() == 8u, "debug: pool reset retires its blocks");
    }
    expectTrue(LeakDetector::liveCount() == 0u, "debug: destroying an allocator releases its arena's records");
    void* raw = fuse::alloc::checkedMalloc(100u);
    expectTrue(LeakDetector::liveBytes() == 100u, "debug: checkedMalloc tracked");
    fuse::alloc::checkedFree(raw);
    expectTrue(LeakDetector::snapshot().liveCount == 0u && LeakDetector::snapshot().unknownFrees == 0u,
               "debug: checkedFree untracks, no unknown frees");
}

struct LogCapture {
    std::mutex mutex;
    std::vector<std::string> errors;
};

void captureSink(fuse::log::Level level, const char* message, void* user) {
    auto* capture = static_cast<LogCapture*>(user);
    if (level >= fuse::log::Level::Error) {
        const std::lock_guard<std::mutex> lock(capture->mutex);
        capture->errors.emplace_back(message);
    }
}

/// A frame's worth of engine work: jobs that allocate and free through the tracked allocators.
void runEngineWork(u32 seed) {
    fuse::alloc::FreeListAllocator heap(64u * 1024u, "b7.work.freelist");
    std::mutex heapMutex;
    std::atomic<u32> done{0};
    fuse::jobs::JobCounter counter;
    constexpr u32 kJobs = 64u;
    counter.add(kJobs);
    for (u32 j = 0; j < kJobs; ++j) {
        fuse::jobs::JobScheduler::instance().submit([&, j]() {
            fuse::alloc::PoolAllocator pool(128u, 16u, "b7.work.pool");
            void* blocks[16];
            for (u32 b = 0; b < 16u; ++b) {
                blocks[b] = pool.alloc({64u + b, 16u, nullptr});
            }
            for (u32 b = 0; b < 16u; ++b) {
                pool.free(blocks[b], 64u + b);
            }
            void* scratch = fuse::alloc::checkedMalloc(32u + (seed + j) % 64u);
            {
                const std::lock_guard<std::mutex> lock(heapMutex);
                void* h = heap.alloc({256u + j, 16u, nullptr});
                heap.free(h, 256u + j);
            }
            fuse::alloc::checkedFree(scratch);
            done.fetch_add(1u);
            counter.signal();
        });
    }
    counter.wait();
    expectTrue(done.load() == kJobs, "engine work: all jobs ran");
}

void testCleanShutdownReportsZero() {
    using fuse::alloc::LeakDetector;
    LeakDetector::clear();
    LogCapture capture;
    auto& logger = fuse::log::Logger::instance();
    logger.setSink(captureSink, &capture);

    fuse::core::initialize();
    runEngineWork(1u);
    fuse::core::shutdown();
    const fuse::alloc::LeakReport clean = LeakDetector::reportLeaks();
    bool leakLogged = false;
    for (const std::string& e : capture.errors) {
        leakLogged = leakLogged || e.find("leak") != std::string::npos;
    }
    expectTrue(clean.liveCount == 0u && clean.unknownFrees == 0u && clean.untracked == 0u && !leakLogged,
               "clean shutdown: leak detector reports zero leaks");

    if constexpr (fuse::alloc::kLeakDetectorEnabled) {
        // Non-vacuous: a deliberate leak through each tracked path is reported at shutdown.
        capture.errors.clear();
        fuse::core::initialize();
        fuse::alloc::PoolAllocator pool(64u, 4u, "b7.leaky.pool");
        void* leakedBlock = pool.alloc({40u, 8u, nullptr});
        void* leakedRaw = fuse::alloc::checkedMalloc(24u);
        fuse::core::shutdown();
        const std::vector<fuse::alloc::LeakRecord> live = LeakDetector::liveRecords();
        bool summary = false;
        u32 detailLines = 0;
        for (const std::string& e : capture.errors) {
            summary = summary || e.find("leak report: 2 live allocations, 64 bytes") != std::string::npos;
            detailLines += e.rfind("leak: ", 0) == 0 ? 1u : 0u;
        }
        std::printf("  deliberate leak: %zu live records, summary %s, %u detail lines\n", live.size(),
                    summary ? "logged" : "missing", detailLines);
        expectTrue(live.size() == 2u && summary && detailLines == 2u,
                   "debug shutdown reports exactly the two deliberately leaked allocations (64 bytes)");
        pool.free(leakedBlock, 40u);
        fuse::alloc::checkedFree(leakedRaw);
        expectTrue(LeakDetector::liveCount() == 0u, "freeing the leaked allocations clears the report");
    }
    logger.setSink(nullptr, nullptr);
}

/// Live blocks from this binary's global operator new counter (-1 where it is not installed).
long long liveHeapBlocks() {
#if defined(FUSE_B7_LINUX)
    return g_liveHeapBlocks.load();
#else
    return -1;
#endif
}

void testInitShutdownSoak() {
    using fuse::alloc::LeakDetector;
    fuse::log::Logger::instance().setSink(quietSink, nullptr);
    constexpr u32 kWarmup = 5u;
    constexpr u32 kCycles = 150u;
    long long afterWarmup = 0;
    u64 detectorMax = 0;
    for (u32 cycle = 0; cycle < kCycles; ++cycle) {
        fuse::core::initialize();
        runEngineWork(cycle);
        fuse::core::shutdown();
        detectorMax = std::max<u64>(detectorMax, LeakDetector::liveCount());
        if (cycle + 1u == kWarmup) {
            afterWarmup = liveHeapBlocks();
        }
    }
    const long long growth = liveHeapBlocks() - afterWarmup;
    std::printf("  soak: %u init/work/shutdown cycles, live heap blocks %lld -> %lld (growth %lld), detector max %llu\n",
                kCycles, afterWarmup, liveHeapBlocks(), growth, static_cast<unsigned long long>(detectorMax));
    expectTrue(growth <= 0, "soak: no heap growth over 145 init/shutdown cycles (global operator new counter)");
    expectTrue(detectorMax == 0u, "soak: leak detector zero after every shutdown");
    fuse::log::Logger::instance().setSink(nullptr, nullptr);
}

// ---- public API ownership scan ------------------------------------------------------------------

/// B7.8 gate "No owning raw pointers in public FUSE APIs" (heuristic): every public header under
/// Source/FUSE/**/include is scanned for factory-style functions returning raw pointers and for naked
/// new/delete expressions. Allocator interfaces (alloc/allocate) hand out raw memory by design and
/// are not factories; new_ban.hpp redefines the keywords on purpose.
void testPublicApiOwnershipScan() {
#if defined(FUSE_B7_SOURCE_ROOT)
    namespace fs = std::filesystem;
    const std::regex factory(R"([A-Za-z0-9_>]\s*\*\s*(create|make|clone|spawn|instantiate|open|load|acquire)[A-Za-z0-9_]*\s*\()");
    const std::regex naked(R"((^|[^A-Za-z0-9_:])(new\s+[A-Za-z_:][A-Za-z0-9_:<>]*\s*[\[({;]|delete(\[\])?\s+[A-Za-z_(*]))");
    u32 headers = 0;
    std::vector<std::string> violations;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(FUSE_B7_SOURCE_ROOT, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path& path = it->path();
        const std::string text = path.generic_string();
        const std::string ext = path.extension().string();
        if (ec || !it->is_regular_file() || (ext != ".hpp" && ext != ".h") ||
            text.find("/include/") == std::string::npos || text.find("/tests/") != std::string::npos ||
            (text.size() >= 23u && text.compare(text.size() - 23u, 23u, "/fuse/alloc/new_ban.hpp") == 0)) {
            continue;
        }
        ++headers;
        std::ifstream in(path);
        std::string line;
        u32 lineNo = 0;
        bool inBlockComment = false;
        while (std::getline(in, line)) {
            ++lineNo;
            std::string code = line;
            if (inBlockComment) {
                const usize close = code.find("*/");
                if (close == std::string::npos) {
                    continue;
                }
                code = code.substr(close + 2u);
                inBlockComment = false;
            }
            const usize open = code.find("/*");
            if (open != std::string::npos && code.find("*/", open) == std::string::npos) {
                code = code.substr(0, open);
                inBlockComment = true;
            }
            const usize comment = code.find("//");
            if (comment != std::string::npos) {
                code = code.substr(0, comment);
            }
            const usize firstNonSpace = code.find_first_not_of(" \t");
            if (firstNonSpace == std::string::npos || code[firstNonSpace] == '#' || code[firstNonSpace] == '*') {
                continue;
            }
            if (std::regex_search(code, factory) || std::regex_search(code, naked)) {
                violations.push_back(text + ":" + std::to_string(lineNo) + ": " + line);
            }
        }
    }
    for (const std::string& v : violations) {
        std::fprintf(stderr, "  owning raw pointer: %s\n", v.c_str());
    }
    std::printf("  ownership scan: %u public headers, %zu violations\n", headers, violations.size());
    expectTrue(headers >= 100u, "ownership scan found the public headers");
    expectTrue(violations.empty(), "no raw-pointer factories or naked new/delete in public headers");

    // The patterns catch what they are meant to catch.
    expectTrue(std::regex_search(std::string("Widget* createWidget(int id);"), factory) &&
                   std::regex_search(std::string("    return new Widget(id);"), naked) &&
                   std::regex_search(std::string("    delete widget;"), naked) &&
                   !std::regex_search(std::string("const Snapshot* newest() const;"), factory) &&
                   !std::regex_search(std::string("Foo(const Foo&) = delete;"), naked),
               "ownership scan patterns self-check");
#endif
}

} // namespace

int main() {
    std::error_code ec;
#if defined(FUSE_B7_LINUX)
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    std::string templ = (base / "fuse_b7_platform_XXXXXX").string();
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    if (mkdtemp(buffer.data()) == nullptr) {
        std::fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }
    g_tempDir = buffer.data();

    testAssertAbortReport();
#if defined(FUSE_B7_ASAN)
    constexpr bool kAsanBuild = true;
#else
    constexpr bool kAsanBuild = false;
#endif
    if (kAsanBuild) {
        std::printf("  AddressSanitizer build: fault-signal crash probes skipped (ASan owns SIGSEGV/SIGFPE)\n");
    } else {
        testNullDereferenceReport();
        testDivideByZeroReport();
        testStackOverflowReport();
        testWorkerThreadCrash();
        testChainsToPreviousHandler();
        testShutdownRestoresHandlers();
        testReportPathAndNote();
    }
#else
    std::printf("crash handler gates: Linux only (Win32 minidump is manual)\n");
#endif

    testLeakTableAgainstLedger();
    testAllocatorHooks();
    testCleanShutdownReportsZero();
    testInitShutdownSoak();
    testPublicApiOwnershipScan();

    if (!g_tempDir.empty() && std::getenv("FUSE_B7_KEEP_REPORTS") == nullptr) {
        std::filesystem::remove_all(g_tempDir, ec);
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "test_b7_platform_gates: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_b7_platform_gates: all passed (leak hooks %s)\n",
                fuse::alloc::kLeakDetectorEnabled ? "on" : "off (release)");
    return 0;
}
