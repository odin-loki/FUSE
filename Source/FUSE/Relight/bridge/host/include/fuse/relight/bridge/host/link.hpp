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
// Ported from dxvk-remix bridge/src/client/d3d9_lss.cpp@0867d3c (InitServer, OnServerExited)

// FUSE Relight RL-2.3: the client end of the bridge — launches the host, sends commands, waits for
// responses, and falls back to in-process plain DXVK when the host dies or hangs.
//
// Semantics kept from upstream (InitServer / OnServerExited, the Present semaphore):
// - the client picks the session name, creates the shared memory and launches the host with it;
// - Syn/Ack/Continue before any command; the client watches for host exit;
// - the client may run ahead of the host by a bounded number of frames (upstream: a named
//   "Present" semaphore with presentSemaphoreMaxFrames; here: at most maxFrameLatency outstanding
//   Present responses).
// Changes (revamp):
// - Host death (crash, kill), a graceful-but-unexpected close, or a hang (no progress within
//   hangTimeoutMs) no longer ends in a crash dialog and a dead renderer (upstream
//   promptCrashReportAndApplyChoice). The link kills the host, builds an in-process backend
//   through the BackendFactory (the D3D9 executor on the passthrough d3d9.dll: x86 vendored DXVK
//   with Relight off), replays the CommandJournal into it, and runs every later command there.
//   The game keeps running; only the Relight features are gone.
// - A host that cannot be started, fails the handshake or runs a different schema starts the link
//   in passthrough directly.
// - Resource uploads above heapThresholdBytes go through the shared heap (the RL-2.2
//   heapChunk/heapBytes fields); the journal keeps the inline form.
#pragma once

#include <fuse/relight/bridge/host/journal.hpp>
#include <fuse/relight/bridge/host/protocol.hpp>
#include <fuse/relight/bridge/ipc/platform.hpp>
#include <fuse/relight/bridge/ipc/session.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fuse::relight::bridge::host {

// The answer to a command (protocol.hpp, replyKind): result = the HRESULT; payload = the assembled
// Reply_Data bytes for read-backs, the encoded Reply_* struct for the other typed replies (decode it
// with schema::cmd::decode), empty for Reply_Result.
struct Response {
    int32_t result = kHrOk;
    std::vector<uint8_t> payload;
};

// Resource-upload commands (RL-2.2: u32 heapChunk, u32 heapBytes, bytes data).
template <class C, class = void>
struct HasHeapFields : std::false_type {};
template <class C>
struct HasHeapFields<C, std::void_t<decltype(C::heapChunk), decltype(C::heapBytes), decltype(C::data)>> : std::true_type {};

// Executes encoded commands in-process. The host process drives the same interface.
class ILocalBackend {
public:
    virtual ~ILocalBackend() = default;
    // Executes one command; fills *out when non-null. Returns the call's HRESULT.
    virtual int32_t execute(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size,
                            Response* out) = 0;
    // The session's shared heap, for uploads with heapBytes != 0 (host side); nullptr in-process.
    virtual void setSharedHeap(ipc::SharedHeap* heap) { (void) heap; }
};

// An upload's bytes: inline `data`, or the shared-heap run {heapChunk, heapBytes} (host side).
struct UploadView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    ipc::HeapRef ref;
};
inline bool resolveUpload(ipc::SharedHeap* heap, uint32_t heapChunk, uint32_t heapBytes,
                          const std::vector<uint8_t>& inlineData, UploadView& out) {
    if (heapBytes == 0) {
        out.data = inlineData.data();
        out.size = inlineData.size();
        return true;
    }
    if (heap == nullptr || !inlineData.empty()) {
        return false;
    }
    out.ref.firstChunk = heapChunk;
    out.ref.bytes = heapBytes;
    out.data = heap->resolve(out.ref);
    out.size = heapBytes;
    return out.data != nullptr;
}
// Returns a resolved heap run to the client (the host releases uploads after the copy).
inline void releaseUpload(ipc::SharedHeap* heap, const UploadView& v) {
    if (heap != nullptr && v.ref.valid()) {
        heap->release(v.ref);
    }
}

// Builds the passthrough backend when the fallback engages; nullptr (with `error`) if it cannot.
using BackendFactory = std::function<std::unique_ptr<ILocalBackend>(std::string& error)>;

enum class LinkMode { Bridged, Passthrough, Failed };

enum class FallbackReason {
    None,
    HostDied,         // the host process exited or was killed (PeerDead)
    HostClosed,       // the host closed the session without being asked (PeerClosed)
    HostHung,         // no response / no queue space within hangTimeoutMs
    SpawnFailed,      // the host executable could not be started
    HandshakeFailed,  // no handshake within the startup timeout, or the host died during it
    VersionMismatch,  // protocol major or schema hash differ
    ProtocolError,    // malformed response, unexpected message
    Requested,        // forced by the caller (tests, a user toggle)
};
const char* toString(FallbackReason r) noexcept;
const char* toString(LinkMode m) noexcept;

struct LinkConfig {
    uint32_t hangTimeoutMs = 15000;       // a response or a queue slot taking longer = host hung
    uint32_t startupTimeoutMs = 20000;    // host start + handshake
    uint32_t maxFrameLatency = 2;         // outstanding Present responses (upstream default 3 frames)
    size_t journalLimitBytes = size_t(128) << 20;
    uint32_t heapThresholdBytes = 256u << 10;  // upload data above this goes through the shared heap
};

// How to start the host process (see protocol.hpp for its command line).
struct HostLaunch {
    std::string hostExe;                 // path of fuse_relight_host.exe
    std::vector<std::string> extraArgs;  // e.g. --d3d9 <path>, --test-fault crash@3
    std::string sessionName;             // empty: "fuse-relight-<pid>-<n>"
    ipc::SessionConfig session;
};

class BridgeLink {
public:
    BridgeLink(const LinkConfig& config, BackendFactory factory);
    ~BridgeLink();
    BridgeLink(const BridgeLink&) = delete;
    BridgeLink& operator=(const BridgeLink&) = delete;

    // Starts the host and handshakes. On failure the link is already in passthrough (or Failed if
    // the backend cannot be built) and the reason is returned; Success = bridged.
    ipc::Result launch(const HostLaunch& launch);
    // Adopts a session that already completed the handshake; killHost ends the host process.
    void attach(std::unique_ptr<ipc::Session> session, std::function<void()> killHost);
    // Switches to passthrough now (replaying the journal).
    void fallBack(FallbackReason reason);

    // Sends one command. Returns its HRESULT: the host's (or the local backend's) when a reply is
    // expected (replyKind, or `out` non-null: then kWantReply is set), kHrOk for fire-and-forget
    // calls, and kHrDeviceLost in Failed mode. Presents always carry kWantReply (the hang probe and
    // frame-latency throttle) but are only waited for once maxFrameLatency are outstanding, or
    // when the device is destroyed (IDirect3DDevice9Ex_Destroy drains them first).
    // Upload commands must carry their data inline (heapBytes == 0): the link moves big ones to the
    // shared heap for the wire and journals the inline form.
    template <class Cmd>
    int32_t call(const Cmd& c, uint32_t handle = 0, Response* out = nullptr, uint16_t flags = 0) {
        encodeTo(c, scratch_);
        if constexpr (HasHeapFields<Cmd>::value) {
            if (mode_ == LinkMode::Bridged && session_ && session_->hasHeap() && c.heapBytes == 0 &&
                c.data.size() > config_.heapThresholdBytes) {
                ipc::HeapRef ref;
                if (heapPut(c.data.data(), c.data.size(), ref)) {
                    Cmd wire = c;
                    wire.heapChunk = ref.firstChunk;
                    wire.heapBytes = ref.bytes;
                    wire.data.clear();
                    encodeTo(wire, wire_);
                    return callRaw(static_cast<uint16_t>(Cmd::kId), flags, handle, scratch_.data(), scratch_.size(), out,
                                   &wire_, &ref);
                }
            }
        }
        return callRaw(static_cast<uint16_t>(Cmd::kId), flags, handle, scratch_.data(), scratch_.size(), out);
    }
    // `data` is the inline encoding (journaled, and executed locally); `wire` (optional) is what goes
    // to the host instead, `heap` the run it references (freed if the send fails).
    int32_t callRaw(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size,
                    Response* out, const std::vector<uint8_t>* wire = nullptr, const ipc::HeapRef* heap = nullptr);

    // Waits for every outstanding Present response (bridged), then sends Bridge_Terminate and
    // closes the session. The host exits with kHostExitOk.
    void shutdown();

    LinkMode mode() const noexcept { return mode_; }
    FallbackReason reason() const noexcept { return reason_; }
    uint32_t fallbacks() const noexcept { return fallbacks_; }
    const std::string& lastError() const noexcept { return error_; }
    const CommandJournal& journal() const noexcept { return journal_; }
    ipc::Session* session() noexcept { return session_.get(); }
    uint64_t replayedCommands() const noexcept { return replayed_; }
    uint64_t fallbackMs() const noexcept { return fallbackMs_; }
    // The host started by launch() (nullptr after attach()); the crash tests kill it.
    ipc::ChildProcess* hostProcess() noexcept { return hostProcess_.get(); }

    // Called once when the fallback engaged (after the replay).
    std::function<void(FallbackReason)> onFallback;
    // Host notifications that arrive between replies (on the calling thread, while it waits):
    // Bridge_InputState (developer-menu UI active: the client blocks game input) and
    // Bridge_DebugMessage.
    std::function<void(bool uiActive)> onInputState;
    std::function<void(uint32_t level, const std::string& text)> onDebugMessage;

private:
    template <class Cmd>
    static void encodeTo(const Cmd& c, std::vector<uint8_t>& out) {
        out.resize(schema::cmd::encodedSize(c));
        ipc::WireWriter w(out.data(), out.size());
        schema::cmd::encode(w, c);
    }
    bool heapPut(const void* p, size_t n, ipc::HeapRef& ref);
    ipc::Result sendToHost(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size);
    ipc::Result awaitResponse(uint32_t uid, ReplyKind kind, Response* out);
    int32_t executeLocal(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size,
                         Response* out);
    static FallbackReason reasonFor(ipc::Result r) noexcept;

    LinkConfig config_;
    BackendFactory factory_;
    std::unique_ptr<ipc::Session> session_;
    std::function<void()> killHost_;
    std::shared_ptr<ipc::ChildProcess> hostProcess_;  // launch(): kept until destruction (tests kill it)
    std::unique_ptr<ILocalBackend> backend_;
    CommandJournal journal_;
    LinkMode mode_ = LinkMode::Bridged;
    FallbackReason reason_ = FallbackReason::None;
    uint32_t fallbacks_ = 0;
    uint64_t replayed_ = 0;
    uint64_t fallbackMs_ = 0;
    std::deque<uint32_t> pendingPresents_;
    std::vector<uint8_t> scratch_;
    std::vector<uint8_t> wire_;
    std::string error_;
    bool inFallback_ = false;
};

}  // namespace fuse::relight::bridge::host
