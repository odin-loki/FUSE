/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix bridge/src/launcher/launcher.cpp@0867d3c

// FUSE Relight RL-2.3: fuse_relight_launcher — starts a game suspended, loads the Relight DLL(s)
// into it with a remote LoadLibraryW, then resumes it.
//
// Semantics kept from upstream NvRemixLauncher: the command line is the game's full path plus its
// arguments; the working directory defaults to the executable's directory (-w / --workdir
// overrides it); the process is created with CREATE_SUSPENDED, the DLL is loaded before the game's
// first instruction, the main thread is resumed, and the launcher waits for the game and returns
// its exit code; injecting across bitness is refused (upstream: ERROR_INVALID_HANDLE from Detours).
// Changes (revamp):
// - No Detours (plan §0.3: dropped): a remote thread calls kernel32!LoadLibraryW with the DLL path
//   written into the suspended process; the thread's exit code (the HMODULE) proves the load, and a
//   failed load terminates the suspended game instead of running it without Relight.
// - Known anti-cheat processes and anti-cheat components next to the game are refused before
//   anything is created (anticheat_list.inc documents the list and the policy).
// - The target's PE machine is read before CreateProcess (x64 launcher for x64 games, the x86
//   build for x86 games) instead of failing inside CreateProcess.
// - Several DLLs may be injected (d3d9.dll and d3d8.dll proxies); UTF-8 paths, wide Win32 calls.
// - --self-test runs the launcher's own acceptance checks (launcher_selftest.cpp).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::bridge::launcher {

// UTF-8 <-> std::filesystem::path in both C++17 and C++20+ (u8path is deprecated in C++20 and
// u8string() returns std::u8string there).
inline std::filesystem::path utf8Path(const std::string& s) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
    return std::filesystem::u8path(s);
#endif
}
inline std::string utf8String(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---- anti-cheat markers (anticheat_list.inc) ----------------------------------------------------
enum class MarkerKind { Process, Directory, File, ExeName };

struct AntiCheatMarker {
    const char* product;
    MarkerKind kind;
    const char* pattern;
    const char* note;
};

struct AntiCheatHit {
    const AntiCheatMarker* marker = nullptr;
    std::string where;  // the process name, or the path that matched
};

const std::vector<AntiCheatMarker>& knownAntiCheatMarkers();
const char* toString(MarkerKind k) noexcept;

// Case-insensitive glob with '*' only.
bool globMatch(std::string_view pattern, std::string_view name) noexcept;

// Directory/File markers in the executable's directory and its parent, and ExeName markers.
std::vector<AntiCheatHit> scanGameInstall(const std::string& exePath);
// Process markers against a list of running image names.
std::vector<AntiCheatHit> scanProcesses(const std::vector<std::string>& imageNames);
// Image names of the running processes (Windows: Toolhelp snapshot; elsewhere: /proc/*/comm).
std::vector<std::string> runningProcessNames();

// ---- PE and command line -------------------------------------------------------------------------
inline constexpr uint16_t kPeMachineI386 = 0x014c;
inline constexpr uint16_t kPeMachineAmd64 = 0x8664;
// Reads IMAGE_FILE_HEADER::Machine. False if the file is not a PE image.
bool readPeMachine(const std::string& path, uint16_t& machine);
// Windows command-line quoting (CommandLineToArgvW rules) of one argument.
std::string quoteArgument(const std::string& arg);

// ---- launching -----------------------------------------------------------------------------------
enum class LaunchStatus {
    Ok,
    RefusedAntiCheat,
    ArchMismatch,
    NotFound,
    CreateFailed,
    InjectFailed,
    Unsupported,  // not Windows
};
const char* toString(LaunchStatus s) noexcept;

// Launcher exit codes (the game's own exit code is returned when it ran).
inline constexpr int kExitRefusedAntiCheat = 9100;
inline constexpr int kExitArchMismatch = 9101;
inline constexpr int kExitNotFound = 9102;
inline constexpr int kExitCreateFailed = 9103;
inline constexpr int kExitInjectFailed = 9104;
inline constexpr int kExitUsage = 9105;
inline constexpr int kExitSelfTestFailed = 9106;
int exitCodeFor(LaunchStatus s) noexcept;

struct LaunchSpec {
    std::string exe;                // game executable (full or relative path)
    std::vector<std::string> args;  // arguments after the executable
    std::string workdir;            // empty: the executable's directory
    std::vector<std::string> dlls;  // injected in order; empty: none (anti-cheat and arch checks still run)
    bool wait = true;               // wait for the game and report its exit code
    uint32_t injectTimeoutMs = 30000;
};

struct LaunchReport {
    LaunchStatus status = LaunchStatus::Ok;
    std::vector<AntiCheatHit> antiCheat;
    std::string message;
    uint32_t pid = 0;
    int exitCode = 0;  // valid when status == Ok and spec.wait
};

// Checks (anti-cheat, PE machine) and, if they pass, creates the process suspended, injects, and
// resumes it. `processNames` overrides runningProcessNames() (the self-test uses it).
LaunchReport launch(const LaunchSpec& spec, const std::vector<std::string>* processNames = nullptr);

// Runs the self-test (Windows): helper binaries next to the launcher executable. Returns 0 on pass.
int runSelfTest(bool verbose);

}  // namespace fuse::relight::bridge::launcher
