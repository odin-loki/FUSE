/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix bridge/src/server/main.cpp@0867d3c (wWinMain, InitializeD3D)

// FUSE Relight RL-2.3: fuse_relight_host.exe (x64) — hosts vendored DXVK + Relight for x86 games.
//
//   fuse_relight_host.exe --bridge-session <name> [--client-pid <pid>] [--d3d9 <path>]
//                         [--open-timeout <ms>] [--relight 0|1] [--test-fault crash@N|hang@N|exit@N]
//                         [--verbose]      (RL-2.2 launch contract; --session is an alias)
//
// Upstream flow kept: attach to the client's shared memory (named by the client), load d3d9.dll
// (upstream: d3d9.dll next to the server, i.e. Remix's DXVK), handshake, then execute commands until
// Bridge_Terminate or client exit, and leave d3d9.dll loaded at exit.
// Changes: the d3d9.dll is loaded before the handshake so a host that cannot render never accepts
// the session (the client falls back to passthrough at once); the session name and options come
// from named arguments (upstream: positional GUID + version string + the game's command line); the
// version check is the session handshake (protocol major + schema hash), not a string compare;
// no Sentry / crash-report helper mode; --test-fault drives the crash-fallback tests.

#include <fuse/relight/bridge/host/d3d9_executor.hpp>
#include <fuse/relight/bridge/host/host_loop.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace fuse::relight::bridge;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: fuse_relight_host.exe --bridge-session <name> [--client-pid <pid>] [--d3d9 <path>]\n"
                 "                             [--open-timeout <ms>] [--relight 0|1] [--test-fault crash@N|hang@N|exit@N]\n"
                 "                             [--verbose]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string sessionName;
    std::string d3d9Path;
    uint32_t openTimeoutMs = 20000;
    int relight = -1;
    bool verbose = false;
    host::HostLoopOptions loop;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if ((a == host::kArgSession || a == "--session") && hasValue) {
            sessionName = argv[++i];
        } else if (a == "--client-pid" && hasValue) {
            ++i;  // the session's control block carries the client pid (supervised in every wait)
        } else if (a == "--d3d9" && hasValue) {
            d3d9Path = argv[++i];
        } else if (a == "--open-timeout" && hasValue) {
            openTimeoutMs = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--relight" && hasValue) {
            relight = std::atoi(argv[++i]) != 0 ? 1 : 0;
        } else if (a == "--test-fault" && hasValue) {
            if (!host::parseHostFault(argv[++i], loop.fault, loop.faultAtPresent)) {
                usage();
                return host::kHostExitUsage;
            }
        } else if (a == "--verbose") {
            verbose = true;
        } else {
            usage();
            return host::kHostExitUsage;
        }
    }
    if (sessionName.empty()) {
        usage();
        return host::kHostExitUsage;
    }
    if (d3d9Path.empty()) {
        d3d9Path = host::thisModuleDirectory() + "d3d9.dll";
    }

    std::unique_ptr<ipc::Session> session;
    ipc::Result r = ipc::Session::open(sessionName, openTimeoutMs, session);
    if (r != ipc::Result::Success) {
        std::fprintf(stderr, "fuse-relight host: cannot open session %s (%s)\n", sessionName.c_str(), ipc::toString(r));
        return host::kHostExitSession;
    }

    host::D3D9Executor executor;
    host::D3D9ExecutorConfig cfg;
    cfg.d3d9Path = d3d9Path;
    cfg.relight = relight;
    cfg.verbose = verbose;
    cfg.ownWindowFallback = true;
    std::string error;
    if (!executor.load(cfg, error)) {
        std::fprintf(stderr, "fuse-relight host: %s\n", error.c_str());
        return host::kHostExitD3D9;  // the session closes with us: the client falls back
    }

    r = session->handshake(openTimeoutMs);
    if (r != ipc::Result::Success) {
        std::fprintf(stderr, "fuse-relight host: handshake failed (%s)%s%s\n", ipc::toString(r),
                     session->peer().rejectReason.empty() ? "" : ": ", session->peer().rejectReason.c_str());
        return host::kHostExitSession;
    }
    if (verbose) {
        std::fprintf(stderr, "fuse-relight host: session %s, client pid %u (%u-bit), d3d9 %s\n", sessionName.c_str(),
                     session->peer().pid, session->peer().pointerBits, d3d9Path.c_str());
    }

    loop.crash = [] { ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u); };
    loop.terminated = [&executor] { return executor.terminated(); };
    host::HostLoopStats stats;
    const int code = host::runHostLoop(*session, executor, loop, &stats);
    const host::D3D9ExecutorStats& es = executor.stats();
    if (verbose || es.unhandled != 0 || es.malformed != 0) {
        std::fprintf(stderr,
                     "fuse-relight host: exit %d: %llu commands, %llu responses, %llu presents, %llu unhandled, "
                     "%llu malformed, %llu unknown handles, %zu live objects\n",
                     code, static_cast<unsigned long long>(stats.commands), static_cast<unsigned long long>(stats.responses),
                     static_cast<unsigned long long>(stats.presents), static_cast<unsigned long long>(es.unhandled),
                     static_cast<unsigned long long>(es.malformed), static_cast<unsigned long long>(es.missingHandles),
                     executor.liveObjects());
        for (const auto& [id, n] : es.unhandledById) {
            std::fprintf(stderr, "  unhandled %s x%llu\n", schema::commandName(id), static_cast<unsigned long long>(n));
        }
    }
    session->close();
    executor.releaseAll();
    std::fflush(stderr);
    return code;
}
