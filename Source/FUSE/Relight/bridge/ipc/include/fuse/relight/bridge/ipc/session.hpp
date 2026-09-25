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
// Ported from dxvk-remix bridge/src/server/main.cpp (handshake), bridge/src/util/{util_bridge_state,util_version}.h@0867d3c

// FUSE Relight RL-2.1: a bridge session — the two channels (client->host, host->client), the shared
// heap, the versioned handshake and peer supervision.
//
// Semantics kept from upstream (Bridge::init, BridgeState, server main.cpp handshake): the client
// creates the shared memory and launches the host; the handshake is Syn (client) -> Ack (host) ->
// Continue (client); the client watches for host exit and the host for client exit; each side has
// a process state (NotInit/Init/Handshaking/Running/.../Exited); commands carry UIDs.
//
// Changes (revamp):
// - Versioned handshake. Upstream compared a version string passed on the host's command line and,
//   separately, per-feature versions exported by the loaded d3d9.dll. Here the shared control block
//   carries a layout version (checked before any queue is touched) and Syn/Ack carry the protocol
//   major/minor, the generated schema hash, the pid and the pointer width. The host rejects a
//   different major or schema hash with Ack{status=Rejected, reason}; both sides then return
//   Result::VersionMismatch instead of running with mismatched command tables.
// - Peer supervision in every wait (wait.hpp) instead of an OS exit callback that toggled a global
//   flag; graceful close is a shared state word (PeerClosed), not a Terminate command race.
// - Channel sizes, queue flavour and heap geometry are chosen by the creator and published in the
//   control block; the host reads them (upstream required both sides to read the same bridge.conf).
// - No singletons: a process may hold several sessions (tests run client and host in one process).
// - Session names are chosen by the caller (upstream: a GUID passed on the command line).
#pragma once

#include <fuse/relight/bridge/ipc/channel.hpp>
#include <fuse/relight/bridge/ipc/platform.hpp>
#include <fuse/relight/bridge/ipc/result.hpp>
#include <fuse/relight/bridge/ipc/shared_heap.hpp>
#include <fuse/relight/bridge/ipc/wait.hpp>
#include <fuse/relight/bridge/ipc/wire.hpp>
#include <fuse/relight/bridge/schema/commands.gen.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace fuse::relight::bridge::ipc {

inline constexpr uint32_t kSessionMagic = 0x31425246u;  // "FRB1"
inline constexpr uint32_t kLayoutVersion = 1;

enum class Role : uint32_t { Client = 0, Host = 1 };

enum class HandshakeStatus : uint32_t { Accepted = 0, Rejected = 1 };

struct SessionConfig {
    QueueKind queueKind = QueueKind::Atomic;
    uint32_t clientToHostSlots = 4096;
    uint32_t clientToHostDataBytes = 8u << 20;
    uint32_t hostToClientSlots = 1024;
    uint32_t hostToClientDataBytes = 1u << 20;
    HeapConfig heap;             // heap.maxBytes == 0: no shared heap
    uint32_t peerCheckMs = 20;   // peer-death detection bound (plus the wait's sleep quantum)
};

// Shared control block at offset 0 of the session region.
struct SessionControl {
    std::atomic<uint32_t> magic;  // written last by the creator (release)
    uint32_t layoutVersion;
    uint32_t regionBytes;
    uint32_t queueKind;
    uint32_t c2hSlots;
    uint32_t c2hDataBytes;
    uint32_t h2cSlots;
    uint32_t h2cDataBytes;
    uint32_t heapChunkBytes;
    uint32_t heapSegmentBytes;
    uint32_t heapMaxBytes;
    uint32_t reserved0[5];
    std::atomic<uint32_t> clientPid;
    std::atomic<uint32_t> hostPid;
    std::atomic<uint32_t> clientState;  // PeerState
    std::atomic<uint32_t> hostState;    // PeerState
    uint32_t reserved1[12];
};
static_assert(sizeof(SessionControl) == 128, "SessionControl layout must match across x86/x64");

struct PeerInfo {
    uint32_t protocolMajor = 0;
    uint32_t protocolMinor = 0;
    uint64_t schemaHash = 0;
    uint32_t pid = 0;
    uint32_t pointerBits = 0;
    std::string buildTag;
    std::string rejectReason;
};

class Session {
public:
    using Incoming = Channel::Incoming;

    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Client: creates the session region (and heap). Result::Exists if the name is in use.
    static Result create(const std::string& name, const SessionConfig& config, std::unique_ptr<Session>& out);
    // Host: waits up to timeoutMs for the client's region, validates it and attaches.
    static Result open(const std::string& name, uint32_t timeoutMs, std::unique_ptr<Session>& out);

    // Syn/Ack/Continue. Success leaves both sides Running.
    Result handshake(uint32_t timeoutMs);

    // Test hook: what this side advertises in Syn/Ack (defaults: the generated schema's values).
    void setAdvertised(uint32_t protocolMajor, uint32_t protocolMinor, uint64_t schemaHash) noexcept;
    const PeerInfo& peer() const noexcept { return peer_; }

    // ---- sending (thread-safe; one message at a time) -------------------------------------------
    template <class Cmd>
    Result send(const Cmd& c, uint32_t handle = 0, uint16_t flags = 0, uint32_t timeoutMs = kInfinite) {
        const size_t size = schema::cmd::encodedSize(c);
        return sendWith(static_cast<uint16_t>(Cmd::kId), flags, handle, size, timeoutMs, [&](uint8_t* p) {
            WireWriter w(p, size);
            schema::cmd::encode(w, c);
        });
    }

    // Zero-copy: fill(uint8_t* payload) writes `size` bytes straight into the ring.
    template <class Fill>
    Result sendWith(uint16_t command, uint16_t flags, uint32_t handle, size_t size, uint32_t timeoutMs, Fill&& fill) {
        if (size > 0xFFFFFFFFu) {
            return Result::TooLarge;
        }
        std::lock_guard<std::mutex> lock(txMutex_);
        Channel::Outgoing out;
        Result r = tx().begin(static_cast<uint32_t>(size), timeoutMs, &wait_, out);
        if (r != Result::Success) {
            return r;
        }
        fill(out.data());
        const uint32_t uid = txUid_ + 1;
        r = tx().publish(out, command, flags, handle, uid, timeoutMs, &wait_);
        if (r == Result::Success) {
            txUid_ = uid;
        }
        return r;
    }
    uint32_t lastSentUid() const noexcept { return txUid_; }

    // ---- receiving (one consumer thread) ---------------------------------------------------------
    Result receive(Incoming& in, uint32_t timeoutMs);
    void release(const Incoming& in);

    // Receives one message and decodes it: visitor(const schema::cmd::X&, const MessageHeader&).
    template <class Visitor>
    Result receiveAndDispatch(Visitor&& visitor, uint32_t timeoutMs) {
        Incoming in;
        Result r = receive(in, timeoutMs);
        if (r != Result::Success) {
            return r;
        }
        const schema::DecodeStatus st =
            schema::dispatch(in.header.command, in.data, in.header.dataSize, visitor, static_cast<const MessageHeader&>(in.header));
        release(in);
        return st == schema::DecodeStatus::Ok ? Result::Success : Result::Malformed;
    }

    // ---- state -----------------------------------------------------------------------------------
    // Graceful close: the peer's waits return PeerClosed once it has drained the queue.
    void close() noexcept;
    bool peerAlive() noexcept;
    PeerState peerState() const noexcept;
    Role role() const noexcept { return role_; }
    const std::string& name() const noexcept { return name_; }
    SharedHeap& heap() noexcept { return heap_; }
    bool hasHeap() const noexcept { return heap_.valid(); }
    const WaitContext& waitContext() const noexcept { return wait_; }
    Channel& tx() noexcept { return role_ == Role::Client ? c2h_ : h2c_; }
    Channel& rx() noexcept { return role_ == Role::Client ? h2c_ : c2h_; }

    static size_t regionBytes(const SessionConfig& config) noexcept;

private:
    explicit Session(Role role) : role_(role) {}
    SessionControl* control() const noexcept { return static_cast<SessionControl*>(mem_.data()); }
    std::atomic<uint32_t>& ownState() noexcept;
    Result expect(schema::CommandId id, Incoming& in, uint32_t deadlineMs);
    void attachPeerIfKnown() noexcept;

    Role role_;
    std::string name_;
    SharedMemory mem_;
    Channel c2h_;
    Channel h2c_;
    SharedHeap heap_;
    PeerProcess peerProcess_;
    WaitContext wait_;
    PeerInfo peer_;
    std::mutex txMutex_;
    uint32_t txUid_ = 0;
    uint32_t advMajor_ = schema::kProtocolMajor;
    uint32_t advMinor_ = schema::kProtocolMinor;
    uint64_t advHash_ = schema::kSchemaHash;
};

}  // namespace fuse::relight::bridge::ipc
