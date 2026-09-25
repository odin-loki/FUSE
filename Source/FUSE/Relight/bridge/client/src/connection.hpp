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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix bridge/src/client/d3d9_lss.cpp (InitServer, OnServerExited),
// bridge/src/client/d3d9_util.h, bridge/src/util/util_bridgecommand.h (ClientMessage,
// WAIT_FOR_*_SERVER_RESPONSE)@0867d3c

// FUSE Relight RL-2.2: the client's connection to the host, on RL-2.3's client link
// (bridge/host/include/fuse/relight/bridge/host/link.hpp: BridgeLink + CommandJournal).
//
// Semantics kept from upstream: the client creates the IPC objects and launches the host on the
// first Direct3DCreate9; Syn/Ack/Continue; most calls are fire-and-forget, calls whose result the
// game needs wait for the host's answer; big resource payloads go through the shared heap.
//
// Changes (revamp):
// - One typed path: every command is a generated schema struct (upstream serialized ad hoc with
//   send_data/send_many and parsed positionally on the server); replies are typed (Reply_*), matched
//   by the request's uid.
// - Host failure no longer ends the game (upstream OnServerExited: crash dialog + TerminateProcess):
//   BridgeLink journals every command the host accepted; when the host dies, hangs, or cannot be
//   started, it replays the journal into an in-process D3D9 executor on the passthrough d3d9.dll
//   (plain vendored DXVK, FUSE_RELIGHT=0; makePassthroughFactory) and runs every later call there.
//   Only when no passthrough d3d9.dll is available do calls fail (D3DERR_DEVICELOST).
// - Present runs ahead of the host by at most LinkConfig::maxFrameLatency frames (upstream: the
//   named Present semaphore).
// - A sync-everything debug mode (FUSE_RELIGHT_BRIDGE_SYNC=1, upstream SendAllServerResponses) makes
//   every command wait for its Reply_Result.
// - Uploads are always handed to the link inline; it moves big ones to the shared heap on the wire
//   and journals the inline form.
#pragma once

#include <fuse/relight/bridge/client/protocol.hpp>
#include <fuse/relight/bridge/host/link.hpp>
#include <fuse/relight/bridge/schema/commands.gen.hpp>

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace fuse::relight::bridge::client {

namespace schema = ::fuse::relight::bridge::schema;
namespace ipc = ::fuse::relight::bridge::ipc;
namespace link = ::fuse::relight::bridge::host;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Every entry point of the client serializes on this lock (D3DCREATE_MULTITHREADED semantics for
// all devices; upstream had a per-device lock only when built WITH_MULTITHREADED_DEVICE).
std::recursive_mutex& apiMutex();
#define FUSE_BRIDGE_LOCK() std::lock_guard<std::recursive_mutex> fuseBridgeLock_(::fuse::relight::bridge::client::apiMutex())

class Connection {
public:
    static Connection& instance();

    // Launches the host and handshakes (idempotent). False only when neither the host nor the
    // passthrough fallback is available.
    bool start();
    bool alive() const noexcept { return link_ != nullptr && link_->mode() != link::LinkMode::Failed; }
    bool bridged() const noexcept { return link_ != nullptr && link_->mode() == link::LinkMode::Bridged; }
    bool syncAll() const noexcept { return syncAll_; }
    uint32_t newHandle() noexcept { return nextHandle_.fetch_add(1, std::memory_order_relaxed); }
    // Graceful close at DLL unload (waits for outstanding Presents, Bridge_Terminate).
    void shutdown() noexcept;
    link::BridgeLink* bridgeLink() noexcept { return link_.get(); }

    // Fire-and-forget; in sync-all mode waits for Reply_Result and returns its hresult.
    template <class Cmd>
    HRESULT post(const Cmd& c, uint32_t handle) {
        if (syncAll_) {
            return call(c, handle, D3DERR_INVALIDCALL);
        }
        send(c, handle, 0);
        return S_OK;
    }

    // Waits for the call's HRESULT (Reply_Result on the host, the executor's result in passthrough).
    template <class Cmd>
    HRESULT call(const Cmd& c, uint32_t handle, HRESULT onFailure) {
        if (!alive()) {
            return onFailure;
        }
        link::Response r;
        const HRESULT hr = static_cast<HRESULT>(link_->call(c, handle, &r));
        return alive() ? hr : onFailure;
    }

    // Typed query: the reply struct R (Reply_Value, Reply_Caps, ...). False when no backend is left.
    template <class Cmd, class R>
    bool query(const Cmd& c, uint32_t handle, R& reply) {
        if (!alive()) {
            return false;
        }
        link::Response r;
        const int32_t hr = link_->call(c, handle, &r);
        if (!alive()) {
            return false;
        }
        reply = R {};
        ipc::WireReader rd(r.payload.data(), r.payload.size());
        if (r.payload.empty() || !schema::cmd::decode(rd, reply)) {
            reply = R {};
        }
        reply.hresult = hr;
        return true;
    }

    // Read-back (Reply_Data): the assembled bytes and the call's hresult.
    template <class Cmd>
    HRESULT readData(const Cmd& c, uint32_t handle, std::vector<uint8_t>& out, HRESULT onFailure) {
        out.clear();
        if (!alive()) {
            return onFailure;
        }
        link::Response r;
        const HRESULT hr = static_cast<HRESULT>(link_->call(c, handle, &r));
        if (!alive()) {
            return onFailure;
        }
        out = std::move(r.payload);
        return hr;
    }

    // Fills heapChunk / heapBytes / data of an upload command with `n` bytes (always inline: the
    // link moves big payloads to the shared heap on the wire and journals the inline form).
    template <class Cmd>
    bool attachData(Cmd& c, const void* p, size_t n) {
        static_assert(link::HasHeapFields<Cmd>::value, "command has no heapChunk/heapBytes/data fields");
        c.heapChunk = 0;
        c.heapBytes = 0;
        c.data.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
        return true;
    }

    // Sends a command without waiting. False when no backend is left.
    template <class Cmd>
    bool send(const Cmd& c, uint32_t handle, uint16_t flags) {
        if (!alive()) {
            return false;
        }
        link_->call(c, handle, nullptr, flags);
        return alive();
    }

private:
    Connection() = default;

    std::unique_ptr<link::BridgeLink> link_;
    std::atomic<uint32_t> nextHandle_ {1};
    bool started_ = false;
    bool syncAll_ = false;
};

inline Connection& bridge() { return Connection::instance(); }

}  // namespace fuse::relight::bridge::client
