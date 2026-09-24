/*
 * Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix bridge/src/client/d3d9_lss.cpp (InitServer, OnServerExited)@0867d3c

// FUSE Relight RL-2.2: client connection (see connection.hpp).
#include "connection.hpp"

#include "window.hpp"

#include <fuse/relight/bridge/host/d3d9_executor.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fuse::relight::bridge::client {

// ---- RL-2.2 x86 layout checks of the RL-2.1 shared-memory structures ------------------------------
// The i686 client and the x64 host map the same memory. RL-2.1 asserts the total sizes; these add
// every field offset and the lock-free property of the shared atomics, evaluated in both the i686
// and the x64 build of this file.
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8, "x86 or x64 only");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "shared 32-bit atomics must be lock-free on x86");
static_assert(alignof(std::atomic<uint32_t>) == 4, "shared atomics are 4-byte aligned words");
static_assert(sizeof(ipc::MessageHeader) == 24 && alignof(ipc::MessageHeader) == 4, "MessageHeader");
static_assert(offsetof(ipc::MessageHeader, command) == 0 && offsetof(ipc::MessageHeader, flags) == 2 &&
                  offsetof(ipc::MessageHeader, handle) == 4 && offsetof(ipc::MessageHeader, uid) == 8 &&
                  offsetof(ipc::MessageHeader, dataOffset) == 12 && offsetof(ipc::MessageHeader, dataSize) == 16 &&
                  offsetof(ipc::MessageHeader, reserved) == 20,
              "MessageHeader field offsets");
static_assert(sizeof(ipc::SessionControl) == 128, "SessionControl");
static_assert(offsetof(ipc::SessionControl, layoutVersion) == 4 && offsetof(ipc::SessionControl, regionBytes) == 8 &&
                  offsetof(ipc::SessionControl, queueKind) == 12 && offsetof(ipc::SessionControl, c2hSlots) == 16 &&
                  offsetof(ipc::SessionControl, c2hDataBytes) == 20 && offsetof(ipc::SessionControl, h2cSlots) == 24 &&
                  offsetof(ipc::SessionControl, h2cDataBytes) == 28 && offsetof(ipc::SessionControl, heapChunkBytes) == 32 &&
                  offsetof(ipc::SessionControl, heapSegmentBytes) == 36 && offsetof(ipc::SessionControl, heapMaxBytes) == 40 &&
                  offsetof(ipc::SessionControl, clientPid) == 64 && offsetof(ipc::SessionControl, hostPid) == 68 &&
                  offsetof(ipc::SessionControl, clientState) == 72 && offsetof(ipc::SessionControl, hostState) == 76,
              "SessionControl field offsets");
static_assert(sizeof(ipc::QueueControl) == 384 && offsetof(ipc::QueueControl, tail) == 128 &&
                  offsetof(ipc::QueueControl, semFree) == 256 && offsetof(ipc::QueueControl, semFilled) == 320,
              "QueueControl field offsets");
static_assert(sizeof(ipc::DataRingControl) == 256 && offsetof(ipc::DataRingControl, writePos) == 128,
              "DataRingControl field offsets");
static_assert(sizeof(ipc::HeapRef) == 8, "HeapRef travels as two u32 command fields");

namespace {

std::string envString(const char* name) {
    char buf[2048];
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n == 0 || n >= sizeof(buf)) ? std::string() : std::string(buf, n);
}

uint32_t envU32(const char* name, uint32_t def) {
    const std::string s = envString(name);
    if (s.empty()) {
        return def;
    }
    return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
}

std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

}  // namespace

void logf(const char* fmt, ...) {
    static const std::string path = envString(kEnvLog);
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ::OutputDebugStringA(buf);
    if (!path.empty()) {
        if (FILE* f = std::fopen(path.c_str(), "a")) {
            std::fputs(buf, f);
            std::fclose(f);
        }
    } else {
        std::fputs(buf, stderr);
        std::fflush(stderr);
    }
}

std::recursive_mutex& apiMutex() {
    static std::recursive_mutex m;
    return m;
}

Connection& Connection::instance() {
    static Connection* c = new Connection();  // never destroyed: objects may outlive static teardown
    return *c;
}

bool Connection::start() {
    FUSE_BRIDGE_LOCK();
    if (started_) {
        return alive();
    }
    started_ = true;
    syncAll_ = envU32(kEnvSyncAll, 0) != 0;

    link::LinkConfig lc;
    lc.hangTimeoutMs = envU32(kEnvTimeoutMs, lc.hangTimeoutMs);
    lc.startupTimeoutMs = envU32(kEnvTimeoutMs, lc.startupTimeoutMs);
    lc.heapThresholdBytes = kInlineDataMax;
    // Fallback: plain vendored DXVK in this process (FUSE_RELIGHT_PASSTHROUGH_D3D9, else
    // <client dir>/fuse_relight_passthrough/d3d9.dll).
    link_.reset(new link::BridgeLink(lc, link::makePassthroughFactory()));
    link_->onFallback = [](link::FallbackReason r) {
        logf("fuse-relight bridge: the host is gone (%s); continuing on in-process DXVK\n", link::toString(r));
    };
    // RL-2.3 hook: the host's developer-menu state (Bridge_InputState) gates game input.
    link_->onInputState = [](bool uiActive) { inputSetUiActive(uiActive); };
    link_->onDebugMessage = [](uint32_t level, const std::string& text) {
        logf("fuse-relight host [%u]: %s\n", level, text.c_str());
    };

    link::HostLaunch hl;
    hl.hostExe = envString(kEnvHostPath);
    if (hl.hostExe.empty()) {
        hl.hostExe = link::thisModuleDirectory() + "fuse_relight_host.exe";
    }
    hl.extraArgs = splitArgs(envString(kEnvHostArgs));
    hl.sessionName = "fuse-relight-bridge-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(::GetTickCount());
    const ipc::Result r = link_->launch(hl);
    if (r != ipc::Result::Success) {
        logf("fuse-relight bridge: host %s not available (%s): %s -> %s\n", hl.hostExe.c_str(), ipc::toString(r),
             link_->lastError().c_str(), link::toString(link_->mode()));
    }
    return alive();
}

void Connection::shutdown() noexcept {
    // Upstream RemixDetach: tell the host to finish (after the outstanding Presents) and let it exit.
    if (link_) {
        link_->shutdown();
    }
}

}  // namespace fuse::relight::bridge::client
