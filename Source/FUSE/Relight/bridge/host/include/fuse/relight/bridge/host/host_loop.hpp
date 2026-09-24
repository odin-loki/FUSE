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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix bridge/src/server/main.cpp@0867d3c (wWinMain command loop, OnClientExited)

// FUSE Relight RL-2.3: the host's command loop, independent of D3D9 so the Linux-native tests can
// drive it with a model backend.
//
// Semantics kept from upstream (ProcessDeviceCommandQueue / wWinMain): one thread pops commands in
// order and executes them; Bridge_Terminate ends the loop; the host exits when the client process
// exits (upstream: an exit callback that terminated the server after a grace period); responses go
// back as Bridge_Response.
// Changes: replies follow the RL-2.2 contract (typed Reply_*, chunked Reply_Data); the backend
// resolves shared-heap uploads through setSharedHeap(); test fault hooks
// (crash / hang / exit at the Nth Present) exercise the client's fallback; the loop returns an exit
// code instead of calling TerminateProcess from a wait callback.
#pragma once

#include <fuse/relight/bridge/host/link.hpp>
#include <fuse/relight/bridge/ipc/session.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace fuse::relight::bridge::host {

enum class HostFault { None, Crash, Hang, Exit };

struct HostLoopOptions {
    HostFault fault = HostFault::None;
    uint32_t faultAtPresent = 0;           // 1-based: the fault fires instead of executing this Present
    std::function<void()> crash;           // HostFault::Crash (default: std::abort)
    std::function<bool()> terminated;      // polled after each command (the executor saw Bridge_Terminate)
};

struct HostLoopStats {
    uint64_t commands = 0;
    uint64_t responses = 0;
    uint64_t presents = 0;
};

// Parses "crash@N", "hang@N", "exit@N". False on anything else.
bool parseHostFault(const std::string& text, HostFault& fault, uint32_t& atPresent);

// Runs until Bridge_Terminate, a client exit/close, or a protocol error. Returns a kHostExit* code.
int runHostLoop(ipc::Session& session, ILocalBackend& backend, const HostLoopOptions& options,
                HostLoopStats* stats = nullptr);

}  // namespace fuse::relight::bridge::host
