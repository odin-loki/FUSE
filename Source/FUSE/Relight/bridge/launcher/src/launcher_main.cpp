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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix bridge/src/launcher/launcher.cpp@0867d3c (PrintUsage, option parsing)

// FUSE Relight RL-2.3: fuse_relight_launcher.exe command line.
//
//   fuse_relight_launcher.exe [-w|--workdir <dir>] [--dll <path>]... [--no-inject] [--no-wait]
//                             [--] <game.exe> [game arguments...]
//   fuse_relight_launcher.exe --self-test [--verbose]
//   fuse_relight_launcher.exe --list-anticheat
//
// Default DLL: d3d9.dll next to the launcher (upstream INJECTION_NAME). Upstream's -i (inject) was
// optional and the default changed the DLL search path instead; here injection is the default and
// --no-inject only runs the checks and starts the game unmodified.

#include <fuse/relight/bridge/launcher/launcher.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>

using namespace fuse::relight::bridge::launcher;

namespace {

void usage() {
    std::printf(
        "Usage:\n"
        "    fuse_relight_launcher.exe [-w <work folder>] [--dll <path>]... [--no-inject] [--no-wait] [--] <game.exe> [args]\n"
        "    fuse_relight_launcher.exe --self-test [--verbose]\n"
        "    fuse_relight_launcher.exe --list-anticheat\n\n"
        "Starts <game.exe> suspended, loads the FUSE Relight DLL(s) into it (default: d3d9.dll next to the\n"
        "launcher), then resumes it and returns its exit code. Games shipping or running a known anti-cheat\n"
        "are refused (exit %d); see --list-anticheat.\n",
        kExitRefusedAntiCheat);
}

std::string launcherDir() {
    char path[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    std::string p(path, n);
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

}  // namespace

int main(int argc, char** argv) {
    LaunchSpec spec;
    bool inject = true;
    bool verbose = false;
    int i = 1;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") {
            ++i;
            break;
        }
        if ((a == "-w" || a == "--workdir") && i + 1 < argc) {
            spec.workdir = argv[++i];
        } else if (a == "--dll" && i + 1 < argc) {
            spec.dlls.push_back(argv[++i]);
        } else if (a == "--no-inject") {
            inject = false;
        } else if (a == "--no-wait") {
            spec.wait = false;
        } else if (a == "-i") {
            // upstream compatibility: injection is the default here
        } else if (a == "--verbose") {
            verbose = true;
        } else if (a == "--self-test") {
            return runSelfTest(verbose || (i + 1 < argc && std::string(argv[i + 1]) == "--verbose")) == 0 ? 0 : kExitSelfTestFailed;
        } else if (a == "--list-anticheat") {
            for (const AntiCheatMarker& m : knownAntiCheatMarkers()) {
                std::printf("%-20s %-16s %-40s %s\n", m.product, toString(m.kind), m.pattern, m.note);
            }
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            usage();
            return kExitUsage;
        } else {
            break;
        }
    }
    if (i >= argc) {
        usage();
        return kExitUsage;
    }
    spec.exe = argv[i++];
    for (; i < argc; ++i) {
        spec.args.push_back(argv[i]);
    }
    if (!inject) {
        spec.dlls.clear();
    } else if (spec.dlls.empty()) {
        spec.dlls.push_back(launcherDir() + "\\d3d9.dll");
    }
    const LaunchReport rep = launch(spec);
    if (rep.status != LaunchStatus::Ok) {
        std::fprintf(stderr, "fuse_relight_launcher: %s: %s\n", toString(rep.status), rep.message.c_str());
        for (const AntiCheatHit& h : rep.antiCheat) {
            std::fprintf(stderr, "  %s: %s %s (%s)\n", h.marker->product, toString(h.marker->kind), h.where.c_str(),
                         h.marker->note);
        }
        return exitCodeFor(rep.status);
    }
    if (verbose) {
        std::fprintf(stderr, "fuse_relight_launcher: pid %u exited with %d\n", rep.pid, rep.exitCode);
    }
    return rep.exitCode;
}
