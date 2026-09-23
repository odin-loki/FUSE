// B7.8 Windows crash handler gate (FUSE_MASTER_PLAN B7.10 row "Crash handler writes valid minidump
// on intentional null dereference — dmp opens in WinDbg").
//
// The test re-launches its own executable (CreateProcessW) with a crash flag. The child installs
// the FUSE crash handlers and crashes; the parent then checks:
//   - the child still dies with the original exception code (0xC0000005 for the null read),
//   - <dir>/fuse_crash_<child pid>.dmp exists and is a structurally valid minidump, parsed here by
//     hand from the file format (MINIDUMP_HEADER 'MDMP' signature + version, bounds-checked stream
//     directory) and cross-checked with dbghelp's own MiniDumpReadDumpStream:
//       ExceptionStream  — EXCEPTION_ACCESS_VIOLATION, read access, fault address 0, the faulting
//                          thread id, exception address inside the crashing function (x64 unwind
//                          table range), thread context RIP == exception address;
//       ThreadListStream — contains the faulting thread with a stack and context in bounds;
//       ModuleListStream — contains this executable, whose image range holds the exception
//                          address and whose TimeDateStamp/SizeOfImage match the PE header (what
//                          WinDbg uses to match symbols);
//       SystemInfoStream — AMD64, at least one processor, a Windows platform id;
//   - <dir>/fuse_crash_<child pid>.txt names the exception, fault address, pid/tid, the dump path,
//     the context note, frames starting at the faulting pc, and a module map with this exe.
// Also: a crash on a worker thread (exception thread != main thread, report still written) and
// abort() via a failed FUSE_VERIFY (SIGABRT handler, synthetic exception code, note carries the
// failed check). Opening the .dmp in WinDbg itself stays a manual check.
#include <fuse/assert.hpp>
#include <fuse/platform/crash_report.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#if defined(_MSC_VER)
#define FUSE_TEST_NOINLINE __declspec(noinline)
#else
#define FUSE_TEST_NOINLINE __attribute__((noinline))
#endif

using fuse::u16;
using fuse::u32;
using fuse::u64;

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

volatile int* volatile g_nullTarget = nullptr;
volatile int g_zero = 0;

} // namespace

// The crash probe: a plain read through a null pointer (fault address 0, read access).
extern "C" FUSE_TEST_NOINLINE int fuse_b7_win32_crash_probe_null_read() {
    return *g_nullTarget + 1;
}

namespace {

constexpr DWORD kAbortExceptionCode = 0xE0465553u; // crash_handler.cpp kAbortExceptionCode
// "MDMP" as stored little-endian in the file (dbghelp.h's MINIDUMP_SIGNATURE is the multichar
// constant 'PMDM', which -Wmultichar rejects).
constexpr fuse::u32 kMinidumpSignature = 0x504D444Du;

#if !(defined(FUSE_NO_ASSERT) && FUSE_NO_ASSERT)
constexpr int kVerifyProbeLine = __LINE__ + 2;
FUSE_TEST_NOINLINE void verifyProbe() {
    FUSE_VERIFY(g_zero == 1, "b7 win32 crash probe verify");
}
#endif

// ---- child side --------------------------------------------------------------------------------

int runChild(const char* mode, const char* directory) {
    // No WER / "program has stopped working" dialog on a real desktop.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    fuse::platform::setCrashReportDirectory(directory);
    if (!fuse::platform::installCrashHandlers()) {
        std::fprintf(stderr, "child: installCrashHandlers failed\n");
        return 2;
    }
    fuse::platform::setCrashContextNote("b7 win32 minidump probe");
    std::fflush(stdout);
    std::fflush(stderr);
    if (std::strcmp(mode, "null") == 0) {
        return fuse_b7_win32_crash_probe_null_read();
    }
    if (std::strcmp(mode, "thread") == 0) {
        int result = 0;
        std::thread worker([&result] { result = fuse_b7_win32_crash_probe_null_read(); });
        worker.join();
        return result;
    }
    if (std::strcmp(mode, "verify") == 0) {
#if !(defined(FUSE_NO_ASSERT) && FUSE_NO_ASSERT)
        fuse::assertion::setSuppressAbortForTests(false);
        verifyProbe();
#endif
        return 0;
    }
    return 3;
}

// ---- parent side -------------------------------------------------------------------------------

struct ChildResult {
    bool started = false;
    DWORD pid = 0;
    DWORD exitCode = 0;
    bool timedOut = false;
};

std::wstring selfPath() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    return std::wstring(buffer, n);
}

std::wstring widen(const std::string& s) {
    std::wstring out;
    for (char c : s) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

ChildResult spawnChild(const char* mode, const std::string& directory) {
    ChildResult result;
    const std::wstring exe = selfPath();
    std::wstring command = L"\"" + exe + L"\" --crash-child " + widen(mode) + L" \"" + widen(directory) + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                          &pi)) {
        std::fprintf(stderr, "CreateProcessW failed: %lu\n", ::GetLastError());
        return result;
    }
    result.started = true;
    result.pid = pi.dwProcessId;
    if (::WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) {
        result.timedOut = true;
        ::TerminateProcess(pi.hProcess, 99);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }
    ::GetExitCodeProcess(pi.hProcess, &result.exitCode);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return result;
}

std::vector<unsigned char> readBinary(const std::string& path) {
    std::vector<unsigned char> data;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return data;
    }
    unsigned char chunk[65536];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        data.insert(data.end(), chunk, chunk + n);
    }
    std::fclose(f);
    return data;
}

std::string readText(const std::string& path) {
    const std::vector<unsigned char> bytes = readBinary(path);
    return std::string(bytes.begin(), bytes.end());
}

std::string field(const std::string& report, const char* name) {
    const std::string key = std::string("\n") + name + ": ";
    const size_t at = report.find(key);
    if (at == std::string::npos) {
        return {};
    }
    const size_t start = at + key.size();
    const size_t end = report.find('\n', start);
    return report.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

u64 parseHex(const std::string& s) {
    return std::strtoull(s.c_str(), nullptr, 16);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Bounds-checked view over the dump bytes.
struct DumpView {
    const std::vector<unsigned char>& bytes;

    bool has(u64 rva, u64 size) const { return rva <= bytes.size() && size <= bytes.size() - rva; }

    template <typename T>
    bool read(u64 rva, T& out) const {
        if (!has(rva, sizeof(T))) {
            return false;
        }
        std::memcpy(&out, bytes.data() + rva, sizeof(T));
        return true;
    }

    std::string readString(RVA rva) const {
        u32 length = 0; // MINIDUMP_STRING: ULONG32 byte length + UTF-16 buffer
        if (!read(rva, length) || !has(static_cast<u64>(rva) + 4u, length)) {
            return {};
        }
        std::string out;
        for (u32 i = 0; i + 1u < length; i += 2u) {
            const unsigned lo = bytes[rva + 4u + i];
            const unsigned hi = bytes[rva + 4u + i + 1u];
            const unsigned ch = lo | (hi << 8u);
            out.push_back(ch < 0x80u ? static_cast<char>(ch) : '?');
        }
        return out;
    }
};

struct DumpFacts {
    bool headerOk = false;
    u32 streamCount = 0;
    bool hasException = false;
    bool hasThreadList = false;
    bool hasModuleList = false;
    bool hasSystemInfo = false;
    u32 exceptionThreadId = 0;
    u32 exceptionCode = 0;
    u64 exceptionAddress = 0;
    u32 exceptionParams = 0;
    u64 exceptionInfo0 = 0;
    u64 exceptionInfo1 = 0;
    u64 contextRip = 0;
    bool contextInBounds = false;
    u32 threadCount = 0;
    bool faultingThreadListed = false;
    bool faultingThreadStackInBounds = false;
    u32 moduleCount = 0;
    bool exeListed = false;
    u64 exeBase = 0;
    u32 exeSize = 0;
    u32 exeTimeDateStamp = 0;
    u16 processorArchitecture = 0xFFFFu;
    u32 processors = 0;
    u32 platformId = 0;
};

DumpFacts parseMinidump(const std::vector<unsigned char>& bytes, const std::string& exeName) {
    DumpFacts facts;
    const DumpView view{bytes};
    MINIDUMP_HEADER header{};
    if (!view.read(0u, header)) {
        return facts;
    }
    facts.headerOk = header.Signature == kMinidumpSignature && (header.Version & 0xFFFFu) == MINIDUMP_VERSION &&
                     header.NumberOfStreams > 0u &&
                     view.has(header.StreamDirectoryRva,
                              static_cast<u64>(header.NumberOfStreams) * sizeof(MINIDUMP_DIRECTORY));
    if (!facts.headerOk) {
        return facts;
    }
    facts.streamCount = header.NumberOfStreams;
    for (u32 i = 0; i < header.NumberOfStreams; ++i) {
        MINIDUMP_DIRECTORY dir{};
        view.read(static_cast<u64>(header.StreamDirectoryRva) + i * sizeof(MINIDUMP_DIRECTORY), dir);
        const MINIDUMP_LOCATION_DESCRIPTOR loc = dir.Location;
        if (!view.has(loc.Rva, loc.DataSize)) {
            continue;
        }
        switch (dir.StreamType) {
        case ExceptionStream: {
            MINIDUMP_EXCEPTION_STREAM ex{};
            if (loc.DataSize < sizeof(ex) || !view.read(loc.Rva, ex)) {
                break;
            }
            facts.hasException = true;
            facts.exceptionThreadId = ex.ThreadId;
            facts.exceptionCode = ex.ExceptionRecord.ExceptionCode;
            facts.exceptionAddress = ex.ExceptionRecord.ExceptionAddress;
            facts.exceptionParams = ex.ExceptionRecord.NumberParameters;
            facts.exceptionInfo0 = ex.ExceptionRecord.ExceptionInformation[0];
            facts.exceptionInfo1 = ex.ExceptionRecord.ExceptionInformation[1];
            facts.contextInBounds = ex.ThreadContext.DataSize >= offsetof(CONTEXT, Rip) + sizeof(DWORD64) &&
                                    view.has(ex.ThreadContext.Rva, ex.ThreadContext.DataSize);
            if (facts.contextInBounds) {
                view.read(static_cast<u64>(ex.ThreadContext.Rva) + offsetof(CONTEXT, Rip), facts.contextRip);
            }
            break;
        }
        case ThreadListStream: {
            u32 count = 0;
            if (!view.read(loc.Rva, count) ||
                loc.DataSize < 4u + static_cast<u64>(count) * sizeof(MINIDUMP_THREAD)) {
                break;
            }
            facts.hasThreadList = true;
            facts.threadCount = count; // entries are matched below, once the exception thread is known
            break;
        }
        case ModuleListStream: {
            u32 count = 0;
            if (!view.read(loc.Rva, count) ||
                loc.DataSize < 4u + static_cast<u64>(count) * sizeof(MINIDUMP_MODULE)) {
                break;
            }
            facts.hasModuleList = true;
            facts.moduleCount = count;
            for (u32 m = 0; m < count; ++m) {
                MINIDUMP_MODULE module{};
                view.read(static_cast<u64>(loc.Rva) + 4u + m * sizeof(MINIDUMP_MODULE), module);
                const std::string name = view.readString(module.ModuleNameRva);
                if (lowerAscii(baseName(name)) == lowerAscii(exeName)) {
                    facts.exeListed = true;
                    facts.exeBase = module.BaseOfImage;
                    facts.exeSize = module.SizeOfImage;
                    facts.exeTimeDateStamp = module.TimeDateStamp;
                }
            }
            break;
        }
        case SystemInfoStream: {
            MINIDUMP_SYSTEM_INFO info{};
            if (loc.DataSize < sizeof(info) || !view.read(loc.Rva, info)) {
                break;
            }
            facts.hasSystemInfo = true;
            facts.processorArchitecture = info.ProcessorArchitecture;
            facts.processors = info.NumberOfProcessors;
            facts.platformId = info.PlatformId;
            break;
        }
        default:
            break;
        }
    }

    // Second pass over the thread list now that the faulting thread id is known.
    for (u32 i = 0; i < header.NumberOfStreams; ++i) {
        MINIDUMP_DIRECTORY dir{};
        view.read(static_cast<u64>(header.StreamDirectoryRva) + i * sizeof(MINIDUMP_DIRECTORY), dir);
        if (dir.StreamType != ThreadListStream || !facts.hasThreadList) {
            continue;
        }
        for (u32 t = 0; t < facts.threadCount; ++t) {
            MINIDUMP_THREAD thread{};
            view.read(static_cast<u64>(dir.Location.Rva) + 4u + t * sizeof(MINIDUMP_THREAD), thread);
            if (thread.ThreadId != facts.exceptionThreadId) {
                continue;
            }
            facts.faultingThreadListed = true;
            facts.faultingThreadStackInBounds =
                thread.Stack.Memory.DataSize > 0u && view.has(thread.Stack.Memory.Rva, thread.Stack.Memory.DataSize) &&
                thread.ThreadContext.DataSize > 0u && view.has(thread.ThreadContext.Rva, thread.ThreadContext.DataSize);
        }
    }
    return facts;
}

/// dbghelp's own reader agrees on the streams (a corrupt directory makes it return FALSE).
bool dbghelpReadsStreams(const std::string& path, u32& exceptionCodeOut) {
    HANDLE file = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    HANDLE mapping = ::CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* base = mapping != nullptr ? ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    bool ok = base != nullptr;
    if (ok) {
        const MINIDUMP_STREAM_TYPE required[] = {ExceptionStream, ThreadListStream, ModuleListStream, SystemInfoStream};
        for (MINIDUMP_STREAM_TYPE type : required) {
            PMINIDUMP_DIRECTORY dir = nullptr;
            PVOID stream = nullptr;
            ULONG size = 0;
            if (!::MiniDumpReadDumpStream(base, type, &dir, &stream, &size) || stream == nullptr || size == 0u) {
                std::fprintf(stderr, "  dbghelp: MiniDumpReadDumpStream(%d) failed\n", static_cast<int>(type));
                ok = false;
                continue;
            }
            if (type == ExceptionStream) {
                exceptionCodeOut = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream)->ExceptionRecord.ExceptionCode;
            }
        }
    }
    if (base != nullptr) {
        ::UnmapViewOfFile(base);
    }
    if (mapping != nullptr) {
        ::CloseHandle(mapping);
    }
    ::CloseHandle(file);
    return ok;
}

std::string makeReportDirectory() {
    char temp[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, temp);
    std::string dir = (n > 0u && n < MAX_PATH) ? std::string(temp, n) : std::string(".\\");
    dir += "fuse_b7_win32_" + std::to_string(::GetCurrentProcessId());
    ::CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string reportBase(const std::string& dir, DWORD pid) {
    return dir + "/fuse_crash_" + std::to_string(pid);
}

void removeReports(const std::string& dir, DWORD pid) {
    ::DeleteFileA((reportBase(dir, pid) + ".dmp").c_str());
    ::DeleteFileA((reportBase(dir, pid) + ".txt").c_str());
}

/// Image-relative [begin, end) of a function from this image's x64 unwind table (.pdata).
bool functionRva(const void* fn, u64& begin, u64& end) {
    // MSVC incremental linking (/INCREMENTAL, CMake's Debug default for link.exe) makes &function
    // the address of an incremental-link-table thunk, `jmp rel32` (E9), which has no unwind entry.
    // Follow it (chained thunks included) to the function body.
    const auto* code = static_cast<const unsigned char*>(fn);
    for (int hops = 0; hops < 4 && code[0] == 0xE9u; ++hops) {
        std::int32_t rel = 0;
        std::memcpy(&rel, code + 1, sizeof(rel));
        code = code + 5 + rel;
    }
    fn = code;
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION entry = ::RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(fn), &imageBase, nullptr);
    if (entry == nullptr) {
        // MSVC emits no .pdata for leaf functions without a frame (the probe is one load + add + ret),
        // so there is no unwind entry to bound it. Fall back to a small window from its first byte;
        // the probe is a handful of instructions, far shorter than this.
        constexpr u64 kLeafProbeWindow = 64u;
        const auto imageStart = reinterpret_cast<const unsigned char*>(::GetModuleHandleW(nullptr));
        if (imageStart == nullptr || code < imageStart) {
            return false;
        }
        begin = static_cast<u64>(code - imageStart);
        end = begin + kLeafProbeWindow;
        return true;
    }
    begin = entry->BeginAddress;
    end = entry->EndAddress;
    return true;
}

u32 selfTimeDateStamp(u32& sizeOfImage) {
    const auto* base = reinterpret_cast<const unsigned char*>(::GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    sizeOfImage = nt->OptionalHeader.SizeOfImage;
    return nt->FileHeader.TimeDateStamp;
}

void checkNullDereference(const std::string& dir, const std::string& exeName) {
    const ChildResult child = spawnChild("null", dir);
    expectTrue(child.started && !child.timedOut, "null deref: child ran to completion");
    std::printf("  null deref: child pid %lu exit code 0x%08lX\n", child.pid, child.exitCode);
    expectTrue(child.exitCode == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION),
               "null deref: child still dies with EXCEPTION_ACCESS_VIOLATION after the report");

    const std::string dmpPath = reportBase(dir, child.pid) + ".dmp";
    const std::string txtPath = reportBase(dir, child.pid) + ".txt";
    const std::vector<unsigned char> dump = readBinary(dmpPath);
    expectTrue(!dump.empty(), "null deref: fuse_crash_<pid>.dmp written");
    std::printf("  null deref: %s (%zu bytes)\n", dmpPath.c_str(), dump.size());

    const DumpFacts f = parseMinidump(dump, exeName);
    expectTrue(f.headerOk, "minidump: header signature 'MDMP', MINIDUMP_VERSION, stream directory in bounds");
    expectTrue(f.hasException, "minidump: ExceptionStream present");
    expectTrue(f.exceptionCode == static_cast<u32>(EXCEPTION_ACCESS_VIOLATION),
               "minidump: exception code EXCEPTION_ACCESS_VIOLATION (0xC0000005)");
    expectTrue(f.exceptionParams >= 2u && f.exceptionInfo0 == 0u, "minidump: access violation is a read");
    expectTrue(f.exceptionParams >= 2u && f.exceptionInfo1 == 0u, "minidump: fault address is 0 (null)");
    expectTrue(f.contextInBounds && f.contextRip == f.exceptionAddress,
               "minidump: exception thread context RIP equals the exception address");
    expectTrue(f.hasThreadList && f.threadCount >= 1u, "minidump: ThreadListStream present");
    expectTrue(f.faultingThreadListed, "minidump: thread list contains the faulting thread");
    expectTrue(f.faultingThreadStackInBounds, "minidump: faulting thread stack + context captured in bounds");
    expectTrue(f.hasModuleList && f.moduleCount >= 2u, "minidump: ModuleListStream present");
    expectTrue(f.exeListed, "minidump: module list contains the crashing executable");
    expectTrue(f.hasSystemInfo, "minidump: SystemInfoStream present");
    expectTrue(f.processorArchitecture == PROCESSOR_ARCHITECTURE_AMD64, "minidump: processor architecture AMD64");
    expectTrue(f.processors >= 1u, "minidump: at least one processor");
    expectTrue(f.platformId == VER_PLATFORM_WIN32_NT, "minidump: platform id WIN32_NT");

    u32 selfSize = 0;
    const u32 selfStamp = selfTimeDateStamp(selfSize);
    expectTrue(f.exeListed && f.exeTimeDateStamp == selfStamp && f.exeSize == selfSize,
               "minidump: exe module TimeDateStamp/SizeOfImage match the PE header (symbol matching)");
    u64 fnBegin = 0;
    u64 fnEnd = 0;
    const bool haveRange = functionRva(reinterpret_cast<const void*>(&fuse_b7_win32_crash_probe_null_read), fnBegin, fnEnd);
    const u64 faultRva = f.exceptionAddress - f.exeBase;
    expectTrue(f.exeListed && f.exceptionAddress >= f.exeBase && f.exceptionAddress < f.exeBase + f.exeSize,
               "minidump: exception address lies inside the executable image");
    expectTrue(haveRange && faultRva >= fnBegin && faultRva < fnEnd,
               "minidump: exception address lies inside fuse_b7_win32_crash_probe_null_read");
    std::printf("  minidump: %u streams, %u threads, %u modules, exception 0x%08X at %s+0x%llx "
                "(probe rva [0x%llx,0x%llx)), fault addr 0x%llx\n",
                f.streamCount, f.threadCount, f.moduleCount, f.exceptionCode, exeName.c_str(),
                static_cast<unsigned long long>(faultRva), static_cast<unsigned long long>(fnBegin),
                static_cast<unsigned long long>(fnEnd), static_cast<unsigned long long>(f.exceptionInfo1));

    u32 dbghelpCode = 0;
    expectTrue(dbghelpReadsStreams(dmpPath, dbghelpCode) && dbghelpCode == static_cast<u32>(EXCEPTION_ACCESS_VIOLATION),
               "minidump: dbghelp MiniDumpReadDumpStream reads Exception/ThreadList/ModuleList/SystemInfo");

    const std::string report = readText(txtPath);
    expectTrue(report.rfind("FUSE crash report\n", 0) == 0, "text report: fuse_crash_<pid>.txt written");
    expectTrue(report.size() > 4u && report.compare(report.size() - 4u, 4u, "end\n") == 0,
               "text report: complete (terminator present)");
    expectTrue(field(report, "exception") == "0x00000000c0000005 EXCEPTION_ACCESS_VIOLATION",
               "text report: exception code and name");
    expectTrue(field(report, "fault_address") == "0x0000000000000000", "text report: fault address 0x0");
    expectTrue(field(report, "access") == "read", "text report: access kind read");
    expectTrue(parseHex(field(report, "pc")) == f.exceptionAddress, "text report: pc equals the dump's exception address");
    expectTrue(field(report, "pid") == std::to_string(child.pid), "text report: pid");
    expectTrue(field(report, "tid") == std::to_string(f.exceptionThreadId), "text report: tid equals the dump's exception thread");
    expectTrue(field(report, "dump") == dmpPath, "text report: names the minidump path");
    expectTrue(field(report, "note") == "b7 win32 minidump probe", "text report: context note");
    const size_t framesAt = report.find("\nframes:\n  ");
    expectTrue(framesAt != std::string::npos &&
                   parseHex(report.substr(framesAt + 11u, 18u)) == f.exceptionAddress,
               "text report: frame 0 is the faulting instruction (unwound from the exception context)");
    const size_t modulesAt = report.find("\nmodules:\n");
    expectTrue(modulesAt != std::string::npos &&
                   lowerAscii(report.substr(modulesAt)).find(lowerAscii(exeName)) != std::string::npos,
               "text report: module map lists the executable");
    // At least two frames: the probe and its caller (runChild) — the unwinder walked past frame 0.
    size_t frameLines = 0;
    for (size_t at = report.find("\n  0x", framesAt); at != std::string::npos && at < modulesAt;
         at = report.find("\n  0x", at + 1u)) {
        ++frameLines;
    }
    expectTrue(frameLines >= 2u, "text report: stack walk reaches the probe's caller");
    removeReports(dir, child.pid);
}

void checkWorkerThreadCrash(const std::string& dir, const std::string& exeName) {
    const ChildResult child = spawnChild("thread", dir);
    expectTrue(child.started && child.exitCode == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION),
               "worker thread crash: child dies with EXCEPTION_ACCESS_VIOLATION");
    const std::string dmpPath = reportBase(dir, child.pid) + ".dmp";
    const DumpFacts f = parseMinidump(readBinary(dmpPath), exeName);
    expectTrue(f.headerOk && f.hasException && f.exceptionCode == static_cast<u32>(EXCEPTION_ACCESS_VIOLATION),
               "worker thread crash: minidump with the access violation written");
    expectTrue(f.faultingThreadListed && f.threadCount >= 2u,
               "worker thread crash: dump lists the faulting worker among several threads");
    const std::string report = readText(reportBase(dir, child.pid) + ".txt");
    expectTrue(field(report, "tid") == std::to_string(f.exceptionThreadId) && !field(report, "tid").empty(),
               "worker thread crash: text report names the faulting thread");
    removeReports(dir, child.pid);
}

void checkVerifyAbort(const std::string& dir, const std::string& exeName) {
#if !(defined(FUSE_NO_ASSERT) && FUSE_NO_ASSERT)
    const ChildResult child = spawnChild("verify", dir);
    std::printf("  failed FUSE_VERIFY: child exit code 0x%08lX\n", child.exitCode);
    expectTrue(child.started && child.exitCode == 3u, "failed FUSE_VERIFY: abort() exit code 3");
    const std::string dmpPath = reportBase(dir, child.pid) + ".dmp";
    const DumpFacts f = parseMinidump(readBinary(dmpPath), exeName);
    const std::string report = readText(reportBase(dir, child.pid) + ".txt");
    std::printf("  failed FUSE_VERIFY: dump header %d, exception stream %d, code 0x%08x, report dump: %s\n",
                f.headerOk ? 1 : 0, f.hasException ? 1 : 0, static_cast<unsigned>(f.exceptionCode),
                field(report, "dump").c_str());
    expectTrue(f.headerOk && f.hasException && f.exceptionCode == kAbortExceptionCode,
               "failed FUSE_VERIFY: minidump records the synthetic abort exception");
    expectTrue(field(report, "exception") == "0x00000000e0465553 abort (SIGABRT)",
               "failed FUSE_VERIFY: text report names abort (SIGABRT)");
    const std::string expectedNote =
        std::string("fatal: b7 win32 crash probe verify (") + __FILE__ + ":" + std::to_string(kVerifyProbeLine) + ")";
    expectTrue(field(report, "note") == expectedNote, "failed FUSE_VERIFY: note carries message, file and line");
    removeReports(dir, child.pid);
#else
    (void)dir;
    (void)exeName;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--crash-child") == 0) {
        return runChild(argv[2], argv[3]);
    }

    const std::string dir = makeReportDirectory();
    const std::wstring exeW = selfPath();
    std::string exe;
    for (wchar_t c : exeW) {
        exe.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    const std::string exeName = baseName(exe);
    std::printf("fuse_core_b7_win32_crash_minidump: report dir %s\n", dir.c_str());

    checkNullDereference(dir, exeName);
    checkWorkerThreadCrash(dir, exeName);
    checkVerifyAbort(dir, exeName);
    ::RemoveDirectoryA(dir.c_str());

    std::printf("fuse_core_b7_win32_crash_minidump: %d/%d checks passed (open a .dmp in WinDbg manually: "
                "set FUSE_CRASH_DIR and run with --crash-child null <dir>)\n",
                g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
