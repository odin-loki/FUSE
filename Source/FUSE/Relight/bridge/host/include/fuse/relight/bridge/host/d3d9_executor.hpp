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
// Ported from dxvk-remix bridge/src/server/main.cpp@0867d3c (ProcessDeviceCommandQueue, InitializeD3D)

// FUSE Relight RL-2.3: replays the bridge command stream on a real d3d9.dll (Windows only).
//
// One executor serves two places:
// - fuse_relight_host.exe (x64): the command stream from the x86 client, on the vendored x64 DXVK
//   with Relight (tap) enabled;
// - the client's crash fallback (link.hpp): the journal and every later command, in-process, on the
//   passthrough d3d9.dll (the x86 build of vendored DXVK with FUSE_RELIGHT=0).
//
// Semantics kept from upstream ProcessDeviceCommandQueue: client-chosen handles map to host COM
// objects (upstream: one unordered_map per object family; here one map with a kind tag); objects
// the device returns (GetBackBuffer, GetSurfaceLevel, ...) are linked to the client's handle;
// Destroy releases the host object; Bridge_UnlinkResource forgets a handle without releasing it;
// Unlock uploads the data the client wrote (inline or from the shared heap, released after the
// copy); surface readbacks (GetRenderTargetData, GetFrontBufferData) return packed rows; HWNDs cross
// the process boundary as 32-bit values; the d3d9.dll is left loaded at exit (upstream: unloading
// DXVK deadlocked on its worker threads).
// Changes (revamp):
// - Decoding is the generated schema::dispatch (no PULL_* macros reading the data queue in lockstep);
//   a malformed payload is rejected before any D3D call.
// - Unknown handles are counted and the call fails with D3DERR_INVALIDCALL instead of dereferencing
//   a default-constructed map entry (upstream `map[handle]` + assert).
// - Refcounts: the executor holds exactly the references it took (Create/Get/AddRef) and releases
//   those on Destroy, instead of upstream's release-until-zero loop.
// - Wire shapes and replies are the RL-2.2 contract (commands.table header): typed Reply_* for
//   queries, chunked Reply_Data for read-backs (Lock/LockRect/LockBox now read host data back;
//   upstream's server ignored Lock), UnlockRect/UnlockBox carry their rect/box and packed rows.
// - DXVK's D3D8 interop (SetD3DCompatibility, UpdateTextureFromBuffer, IsSupportedSurfaceFormat)
//   is reached through its ABI interfaces, so the x86 d3d8.dll-over-bridge path works.
// - Direct3DCreate9 honours `ex`: each client handle gets its own plain or Ex IDirect3D9 (DXVK gives
//   an Ex interface D3D9Ex rules, e.g. no D3DPOOL_MANAGED). Upstream had one global interface.
// - Remix API rows (RemixApi_*) are not executed yet (RL-6.2); they count as unhandled.
#pragma once

#include <fuse/relight/bridge/host/link.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace fuse::relight::bridge::host {

struct D3D9ExecutorConfig {
    std::string d3d9Path;  // full path of the d3d9.dll to load
    // FUSE_RELIGHT for the loaded DXVK: 1 = Relight (tap per its options), 0 = plain DXVK passthrough,
    // -1 = leave the environment as inherited.
    int relight = -1;
    bool verbose = false;
    // Host only: when a device cannot be created on the client's window from this process (Wine 9:
    // winevulkan has no cross-process Vulkan surfaces; Windows has them), present into a host
    // window of the back-buffer size instead. Rendering and read-backs are unaffected.
    bool ownWindowFallback = false;
};

struct D3D9ExecutorStats {
    uint64_t executed = 0;
    uint64_t unhandled = 0;
    uint64_t malformed = 0;
    uint64_t missingHandles = 0;
    uint64_t failedCalls = 0;
    std::map<uint16_t, uint64_t> unhandledById;
};

class D3D9Executor final : public ILocalBackend {
public:
    D3D9Executor();
    ~D3D9Executor() override;
    D3D9Executor(const D3D9Executor&) = delete;
    D3D9Executor& operator=(const D3D9Executor&) = delete;

    // Loads the d3d9.dll and creates the IDirect3D9(Ex) object (upstream InitializeD3D).
    bool load(const D3D9ExecutorConfig& config, std::string& error);

    int32_t execute(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size,
                    Response* out) override;
    void setSharedHeap(ipc::SharedHeap* heap) override;

    bool terminated() const noexcept;  // Bridge_Terminate was executed
    const D3D9ExecutorStats& stats() const noexcept;
    size_t liveObjects() const noexcept;
    // Releases every object (devices last). The destructor does it too.
    void releaseAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The fallback backend of link.hpp: an executor on the passthrough d3d9.dll with FUSE_RELIGHT=0.
// d3d9Path empty: passthroughD3D9Path().
BackendFactory makePassthroughFactory(const std::string& d3d9Path = std::string());

// Where the passthrough d3d9.dll is looked up: FUSE_RELIGHT_PASSTHROUGH_D3D9 (environment), else
// <directory of the module containing this code>/fuse_relight_passthrough/d3d9.dll.
std::string passthroughD3D9Path();

// Directory of the module (exe or dll) that contains this code, with a trailing separator.
std::string thisModuleDirectory();

}  // namespace fuse::relight::bridge::host
