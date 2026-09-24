// FUSE Relight RL-2.3: fuse_relight_launcher --self-test.
// Copyright (c) 2026 FUSE contributors (MIT). New code (upstream's launcher had a _DEBUG-only
// export-ordinal check of the DLL, which the remote LoadLibraryW does not need).
//
// Uses two helpers built next to the launcher:
//   fuse_relight_launcher_selftest_target.exe  writes ran.txt next to itself, reports whether the
//                                             probe DLL was loaded before main() (exit code), or
//                                             idles (--sleep <ms>, the fake anti-cheat process)
//   fuse_relight_launcher_selftest_probe.dll   records its DLL_PROCESS_ATTACH
// Checks, in a scratch directory under %TEMP%:
//   1. the marker table is well formed; glob, PE and quoting helpers behave;
//   2. injection: the probe is loaded into the suspended target before its main() runs, the
//      arguments arrive intact, and the target's exit code comes back through the launcher;
//   3. refusal by install layout: EasyAntiCheat/ next to the game, a *_BE.exe BattlEye launcher,
//      PunkBuster's pb/pbcl.dll, BattlEye/ in the parent directory -> RefusedAntiCheat, and the
//      game never ran (no ran.txt);
//   4. refusal by running process: a target copy named EasyAntiCheat.exe is started; a clean game
//      is refused while it runs (real Toolhelp snapshot) and accepted after it exits;
//   5. an x86 (PE32) executable is refused by the x64 launcher (and vice versa) before creation;
//   6. a DLL that cannot be loaded terminates the suspended game: InjectFailed, no ran.txt.

#include <fuse/relight/bridge/launcher/launcher.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fuse::relight::bridge::launcher {

namespace {

namespace fs = std::filesystem;

int g_failures = 0;
bool g_verbose = false;

void expect(bool cond, const std::string& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else if (g_verbose) {
        std::fprintf(stderr, "ok: %s\n", what.c_str());
    }
}

std::string exeDir() {
    char path[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    std::string p(path, n);
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
}

// A minimal PE32 header (machine i386) — enough for the architecture check, never executed.
void writeFakePe32(const fs::path& p) {
    std::vector<char> img(0x100, 0);
    img[0] = 'M';
    img[1] = 'Z';
    const uint32_t off = 0x80;
    std::memcpy(img.data() + 0x3c, &off, 4);
    std::memcpy(img.data() + off, "PE\0\0", 4);
    const uint16_t machine = kPeMachineI386;
    std::memcpy(img.data() + off + 4, &machine, 2);
    std::ofstream(p, std::ios::binary).write(img.data(), static_cast<std::streamsize>(img.size()));
}

LaunchReport run(const fs::path& exe, const std::vector<std::string>& args, const std::vector<std::string>& dlls,
                 const std::vector<std::string>* processes) {
    LaunchSpec spec;
    spec.exe = utf8String(exe);
    spec.args = args;
    spec.dlls = dlls;
    spec.injectTimeoutMs = 20000;
    const LaunchReport rep = launch(spec, processes);
    if (g_verbose) {
        std::fprintf(stderr, "  launch %s -> %s (exit %d) %s\n", spec.exe.c_str(), toString(rep.status), rep.exitCode,
                     rep.message.c_str());
    }
    return rep;
}

}  // namespace

int runSelfTest(bool verbose) {
    g_verbose = verbose;
    g_failures = 0;
    const fs::path bin = utf8Path(exeDir());
    const fs::path target = bin / "fuse_relight_launcher_selftest_target.exe";
    const fs::path probe = bin / "fuse_relight_launcher_selftest_probe.dll";
    if (!fs::exists(target) || !fs::exists(probe)) {
        std::fprintf(stderr, "self-test: helper binaries missing next to the launcher (%s)\n", utf8String(bin).c_str());
        return 1;
    }
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("fuse_relight_launcher_selftest_" + std::to_string(::GetCurrentProcessId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const std::vector<std::string> noProcesses;  // layout checks do not depend on what else runs

    // 1. Table and helpers.
    const auto& markers = knownAntiCheatMarkers();
    expect(markers.size() >= 30, "marker table has the documented entries");
    bool wellFormed = true;
    for (const AntiCheatMarker& m : markers) {
        wellFormed = wellFormed && m.product[0] != '\0' && m.pattern[0] != '\0' && m.note[0] != '\0';
    }
    expect(wellFormed, "every marker has a product, a pattern and a note");
    expect(globMatch("*_BE.exe", "Arma2OA_be.EXE") && !globMatch("*_BE.exe", "Arma2OA.exe"), "glob matching");
    expect(quoteArgument("a b\\\"c\\") == "\"a b\\\\\\\"c\\\\\"", "argument quoting");
    uint16_t machine = 0;
    expect(readPeMachine(utf8String(target), machine) && machine == (sizeof(void*) == 8 ? kPeMachineAmd64 : kPeMachineI386),
           "PE machine of the target");

    // 2. Injection + argument passing + exit code.
    const fs::path clean = root / "clean" / "game.exe";
    fs::create_directories(clean.parent_path(), ec);
    fs::copy_file(target, clean, ec);
    LaunchReport r = run(clean, {"--expect-probe", "arg with spaces", "q\"uote"}, {utf8String(probe)}, &noProcesses);
    expect(r.status == LaunchStatus::Ok && r.exitCode == 0,
           "probe DLL injected before main, arguments intact (exit " + std::to_string(r.exitCode) + ")");
    expect(fs::exists(clean.parent_path() / "ran.txt"), "the injected game ran");
    fs::remove(clean.parent_path() / "ran.txt", ec);
    r = run(clean, {"--expect-no-probe"}, {}, &noProcesses);
    expect(r.status == LaunchStatus::Ok && r.exitCode == 0, "launch without DLLs runs the game unmodified");
    r = run(clean, {"--exit", "42"}, {utf8String(probe)}, &noProcesses);
    expect(r.status == LaunchStatus::Ok && r.exitCode == 42, "the game's exit code is returned");
    fs::remove(clean.parent_path() / "ran.txt", ec);

    // 3. Refusal by install layout.
    struct Layout {
        const char* name;
        const char* exe;
        const char* marker;  // relative to the game directory ("../x" = parent)
        bool directory;
    };
    const Layout layouts[] = {
        {"eac", "game.exe", "EasyAntiCheat", true},
        {"battleye_exe", "Game_BE.exe", nullptr, false},
        {"punkbuster", "game.exe", "pb/pbcl.dll", false},
        {"battleye_parent", "bin/game.exe", "BattlEye", true},
    };
    for (const Layout& l : layouts) {
        const fs::path exe = root / l.name / utf8Path(l.exe);
        fs::create_directories(exe.parent_path(), ec);
        fs::copy_file(target, exe, ec);
        if (l.marker != nullptr) {
            const fs::path m = root / l.name / utf8Path(l.marker);
            if (l.directory) {
                fs::create_directories(m, ec);
            } else {
                touch(m);
            }
        }
        r = run(exe, {}, {utf8String(probe)}, &noProcesses);
        expect(r.status == LaunchStatus::RefusedAntiCheat && !r.antiCheat.empty(),
               std::string("refused next to anti-cheat layout '") + l.name + "'");
        expect(!fs::exists(exe.parent_path() / "ran.txt"), std::string("refused game never ran (") + l.name + ")");
    }

    // 4. Refusal by a running anti-cheat process (real process list).
    {
        const fs::path fake = root / "service" / "EasyAntiCheat.exe";
        fs::create_directories(fake.parent_path(), ec);
        fs::copy_file(target, fake, ec);
        const std::wstring cmd = L"\"" + fake.wstring() + L"\" --sleep 60000";
        std::wstring mutableCmd = cmd;
        STARTUPINFOW si {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi {};
        const bool started = ::CreateProcessW(fake.wstring().c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0,
                                              nullptr, nullptr, &si, &pi) != 0;
        expect(started, "fake anti-cheat service started");
        if (started) {
            bool seen = false;
            for (int i = 0; i < 100 && !seen; ++i) {
                for (const std::string& n : runningProcessNames()) {
                    seen = seen || globMatch("EasyAntiCheat.exe", n);
                }
                if (!seen) {
                    ::Sleep(50);
                }
            }
            expect(seen, "the process snapshot lists the fake service");
            r = run(clean, {"--expect-probe"}, {utf8String(probe)}, nullptr);
            expect(r.status == LaunchStatus::RefusedAntiCheat, "refused while an anti-cheat service runs");
            expect(!fs::exists(clean.parent_path() / "ran.txt"), "game did not run while the service ran");
            ::TerminateProcess(pi.hProcess, 0);
            ::WaitForSingleObject(pi.hProcess, 5000);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            r = run(clean, {"--expect-probe"}, {utf8String(probe)}, nullptr);
            expect(r.status == LaunchStatus::Ok && r.exitCode == 0, "accepted once the service is gone");
            fs::remove(clean.parent_path() / "ran.txt", ec);
        }
    }

    // 5. Architecture mismatch.
    {
        const fs::path pe32 = root / "old" / "old.exe";
        fs::create_directories(pe32.parent_path(), ec);
        writeFakePe32(pe32);
        r = run(pe32, {}, {utf8String(probe)}, &noProcesses);
        expect(sizeof(void*) == 8 ? r.status == LaunchStatus::ArchMismatch : r.status != LaunchStatus::Ok,
               "PE32 target refused by the x64 launcher");
    }

    // 6. A DLL that cannot load: the suspended game is terminated, never runs.
    {
        r = run(clean, {"--expect-probe"}, {utf8String((root / "missing.dll"))}, &noProcesses);
        expect(r.status == LaunchStatus::InjectFailed, "missing DLL -> InjectFailed");
        expect(!fs::exists(clean.parent_path() / "ran.txt"), "game terminated before main after a failed injection");
    }

    fs::remove_all(root, ec);
    std::fprintf(stderr, "fuse_relight_launcher self-test: %s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                 g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

}  // namespace fuse::relight::bridge::launcher
