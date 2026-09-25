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
// Ported from dxvk-remix bridge/src/server/main.cpp (handshake), bridge/src/util/{util_bridge_state,util_version}.h@0867d3c

#include <fuse/relight/bridge/ipc/session.hpp>

#include <new>

namespace fuse::relight::bridge::ipc {

namespace {

constexpr size_t kControlBytes = 256;  // SessionControl + padding: channels start 256-aligned

ChannelConfig c2hConfig(const SessionConfig& c) { return {c.queueKind, c.clientToHostSlots, c.clientToHostDataBytes}; }
ChannelConfig h2cConfig(const SessionConfig& c) { return {c.queueKind, c.hostToClientSlots, c.hostToClientDataBytes}; }

bool validConfig(const SessionConfig& c) {
    return isPowerOfTwo(c.clientToHostSlots) && isPowerOfTwo(c.hostToClientSlots) &&
           isPowerOfTwo(c.clientToHostDataBytes) && isPowerOfTwo(c.hostToClientDataBytes) &&
           c.clientToHostDataBytes >= 64 && c.hostToClientDataBytes >= 64 &&
           (c.queueKind == QueueKind::Atomic || c.queueKind == QueueKind::Blocking) && c.peerCheckMs > 0;
}

uint32_t remainingMs(uint64_t start, uint32_t timeoutMs) {
    if (timeoutMs == kInfinite) {
        return kInfinite;
    }
    const uint64_t elapsed = nowMs() - start;
    return elapsed >= timeoutMs ? 0 : static_cast<uint32_t>(timeoutMs - elapsed);
}

}  // namespace

size_t Session::regionBytes(const SessionConfig& config) noexcept {
    return kControlBytes + Channel::bytesFor(c2hConfig(config)) + Channel::bytesFor(h2cConfig(config));
}

Session::~Session() { close(); }

std::atomic<uint32_t>& Session::ownState() noexcept {
    return role_ == Role::Client ? control()->clientState : control()->hostState;
}

Result Session::create(const std::string& name, const SessionConfig& config, std::unique_ptr<Session>& out) {
    if (!validConfig(config)) {
        return Result::Malformed;
    }
    std::unique_ptr<Session> s(new Session(Role::Client));
    s->name_ = name;
    const size_t bytes = regionBytes(config);
    if (bytes > 0xFFFFFFFFu) {
        return Result::TooLarge;
    }
    Result r = s->mem_.create(name, bytes);
    if (r != Result::Success) {
        return r;
    }
    auto* base = static_cast<uint8_t*>(s->mem_.data());
    SessionControl* ctl = new (base) SessionControl();
    ctl->layoutVersion = kLayoutVersion;
    ctl->regionBytes = static_cast<uint32_t>(bytes);
    ctl->queueKind = static_cast<uint32_t>(config.queueKind);
    ctl->c2hSlots = config.clientToHostSlots;
    ctl->c2hDataBytes = config.clientToHostDataBytes;
    ctl->h2cSlots = config.hostToClientSlots;
    ctl->h2cDataBytes = config.hostToClientDataBytes;
    ctl->heapChunkBytes = config.heap.chunkBytes;
    ctl->heapSegmentBytes = config.heap.segmentBytes;
    ctl->heapMaxBytes = config.heap.maxBytes;
    ctl->clientPid.store(currentPid(), std::memory_order_relaxed);
    ctl->hostPid.store(0, std::memory_order_relaxed);
    ctl->clientState.store(static_cast<uint32_t>(PeerState::Attached), std::memory_order_relaxed);
    ctl->hostState.store(static_cast<uint32_t>(PeerState::None), std::memory_order_relaxed);

    const ChannelConfig c2h = c2hConfig(config);
    r = s->c2h_.initialize(base + kControlBytes, c2h, name + ".c2h");
    if (r == Result::Success) {
        r = s->h2c_.initialize(base + kControlBytes + Channel::bytesFor(c2h), h2cConfig(config), name + ".h2c");
    }
    if (r == Result::Success && config.heap.maxBytes != 0) {
        r = s->heap_.create(name, config.heap);
    }
    if (r != Result::Success) {
        return r;
    }
    s->wait_.peer = &s->peerProcess_;
    s->wait_.peerState = &ctl->hostState;
    s->wait_.peerCheckMs = config.peerCheckMs;
    s->heap_.setWaitContext(&s->wait_);
    ctl->magic.store(kSessionMagic, std::memory_order_release);
    out = std::move(s);
    return Result::Success;
}

Result Session::open(const std::string& name, uint32_t timeoutMs, std::unique_ptr<Session>& out) {
    std::unique_ptr<Session> s(new Session(Role::Host));
    s->name_ = name;
    const uint64_t start = nowMs();
    Result r;
    for (;;) {
        r = s->mem_.open(name, sizeof(SessionControl));
        if (r == Result::Success) {
            if (static_cast<SessionControl*>(s->mem_.data())->magic.load(std::memory_order_acquire) == kSessionMagic) {
                break;
            }
            s->mem_.reset();  // the creator has not finished initializing it
            r = Result::NotFound;
        }
        if (r != Result::NotFound) {
            return r;
        }
        if (remainingMs(start, timeoutMs) == 0) {
            return Result::Timeout;
        }
        sleepMs(2);
    }
    // POSIX: both sides have it mapped now, so remove the name (nothing leaks if either dies).
    s->mem_.unlinkName();
    SessionControl* ctl = s->control();
    if (ctl->layoutVersion != kLayoutVersion) {
        return Result::VersionMismatch;
    }
    SessionConfig config;
    config.queueKind = static_cast<QueueKind>(ctl->queueKind);
    config.clientToHostSlots = ctl->c2hSlots;
    config.clientToHostDataBytes = ctl->c2hDataBytes;
    config.hostToClientSlots = ctl->h2cSlots;
    config.hostToClientDataBytes = ctl->h2cDataBytes;
    if (!validConfig(config) || regionBytes(config) != ctl->regionBytes || s->mem_.size() < ctl->regionBytes) {
        return Result::Malformed;
    }
    auto* base = static_cast<uint8_t*>(s->mem_.data());
    const ChannelConfig c2h = c2hConfig(config);
    r = s->c2h_.attach(base + kControlBytes, c2h, name + ".c2h");
    if (r == Result::Success) {
        r = s->h2c_.attach(base + kControlBytes + Channel::bytesFor(c2h), h2cConfig(config), name + ".h2c");
    }
    if (r == Result::Success && ctl->heapMaxBytes != 0) {
        r = s->heap_.open(name);
    }
    if (r != Result::Success) {
        return r;
    }
    s->peerProcess_.attach(ctl->clientPid.load(std::memory_order_acquire));
    s->wait_.peer = &s->peerProcess_;
    s->wait_.peerState = &ctl->clientState;
    s->heap_.setWaitContext(&s->wait_);
    ctl->hostPid.store(currentPid(), std::memory_order_release);
    ctl->hostState.store(static_cast<uint32_t>(PeerState::Attached), std::memory_order_release);
    out = std::move(s);
    return Result::Success;
}

void Session::setAdvertised(uint32_t protocolMajor, uint32_t protocolMinor, uint64_t schemaHash) noexcept {
    advMajor_ = protocolMajor;
    advMinor_ = protocolMinor;
    advHash_ = schemaHash;
}

void Session::attachPeerIfKnown() noexcept {
    if (role_ == Role::Client && !peerProcess_.attached()) {
        const uint32_t pid = control()->hostPid.load(std::memory_order_acquire);
        if (pid != 0) {
            peerProcess_.attach(pid);
        }
    }
}

Result Session::receive(Incoming& in, uint32_t timeoutMs) {
    attachPeerIfKnown();
    return rx().receive(in, timeoutMs, &wait_);
}

void Session::release(const Incoming& in) { rx().release(in); }

// Receives the next message and requires it to be `id` (handshake steps). While the client has not
// seen the host attach yet, waits in short slices so the host's pid is picked up for supervision.
Result Session::expect(schema::CommandId id, Incoming& in, uint32_t timeoutMs) {
    const uint64_t start = nowMs();
    for (;;) {
        const uint32_t left = remainingMs(start, timeoutMs);
        const uint32_t slice = peerProcess_.attached() ? left : (left < wait_.peerCheckMs ? left : wait_.peerCheckMs);
        Result r = receive(in, slice);
        if (r == Result::Timeout && remainingMs(start, timeoutMs) != 0) {
            continue;
        }
        if (r != Result::Success) {
            return r;
        }
        if (in.header.command != static_cast<uint16_t>(id)) {
            release(in);
            return Result::Malformed;
        }
        return Result::Success;
    }
}

Result Session::handshake(uint32_t timeoutMs) {
    const uint64_t start = nowMs();
    ownState().store(static_cast<uint32_t>(PeerState::Attached), std::memory_order_release);
    Incoming in;
    if (role_ == Role::Client) {
        schema::cmd::Bridge_Syn syn;
        syn.protocolMajor = advMajor_;
        syn.protocolMinor = advMinor_;
        syn.schemaHash = advHash_;
        syn.pid = currentPid();
        syn.pointerBits = static_cast<uint32_t>(sizeof(void*) * 8);
        syn.buildTag = "fuse-relight";
        Result r = send(syn, 0, 0, remainingMs(start, timeoutMs));
        if (r != Result::Success) {
            return r;
        }
        r = expect(schema::CommandId::Bridge_Ack, in, remainingMs(start, timeoutMs));
        if (r != Result::Success) {
            return r;
        }
        schema::cmd::Bridge_Ack ack;
        WireReader rd(in.data, in.header.dataSize);
        const bool ok = schema::cmd::decode(rd, ack) && rd.atEnd();
        release(in);
        if (!ok) {
            return Result::Malformed;
        }
        peer_.protocolMajor = ack.protocolMajor;
        peer_.protocolMinor = ack.protocolMinor;
        peer_.schemaHash = ack.schemaHash;
        peer_.pid = ack.pid;
        peer_.rejectReason = ack.reason;
        if (ack.status != static_cast<uint32_t>(HandshakeStatus::Accepted) || ack.protocolMajor != advMajor_ ||
            ack.schemaHash != advHash_) {
            close();
            return Result::VersionMismatch;
        }
        r = send(schema::cmd::Bridge_Continue {}, 0, 0, remainingMs(start, timeoutMs));
        if (r != Result::Success) {
            return r;
        }
    } else {
        Result r = expect(schema::CommandId::Bridge_Syn, in, remainingMs(start, timeoutMs));
        if (r != Result::Success) {
            return r;
        }
        schema::cmd::Bridge_Syn syn;
        WireReader rd(in.data, in.header.dataSize);
        const bool ok = schema::cmd::decode(rd, syn) && rd.atEnd();
        release(in);
        if (!ok) {
            return Result::Malformed;
        }
        peer_.protocolMajor = syn.protocolMajor;
        peer_.protocolMinor = syn.protocolMinor;
        peer_.schemaHash = syn.schemaHash;
        peer_.pid = syn.pid;
        peer_.pointerBits = syn.pointerBits;
        peer_.buildTag = syn.buildTag;
        schema::cmd::Bridge_Ack ack;
        ack.protocolMajor = advMajor_;
        ack.protocolMinor = advMinor_;
        ack.schemaHash = advHash_;
        ack.pid = currentPid();
        const bool accepted = syn.protocolMajor == advMajor_ && syn.schemaHash == advHash_;
        ack.status = static_cast<uint32_t>(accepted ? HandshakeStatus::Accepted : HandshakeStatus::Rejected);
        if (!accepted) {
            ack.reason = syn.protocolMajor != advMajor_ ? "protocol major version differs" : "command schema hash differs";
        }
        r = send(ack, 0, 0, remainingMs(start, timeoutMs));
        if (r != Result::Success) {
            return r;
        }
        if (!accepted) {
            close();
            return Result::VersionMismatch;
        }
        r = expect(schema::CommandId::Bridge_Continue, in, remainingMs(start, timeoutMs));
        if (r != Result::Success) {
            return r;
        }
        release(in);
    }
    ownState().store(static_cast<uint32_t>(PeerState::Running), std::memory_order_release);
    return Result::Success;
}

void Session::close() noexcept {
    if (mem_.valid()) {
        ownState().store(static_cast<uint32_t>(PeerState::Closed), std::memory_order_release);
    }
}

bool Session::peerAlive() noexcept {
    attachPeerIfKnown();
    return peerProcess_.attached() && peerProcess_.alive();
}

PeerState Session::peerState() const noexcept {
    const SessionControl* ctl = control();
    return static_cast<PeerState>((role_ == Role::Client ? ctl->hostState : ctl->clientState).load(std::memory_order_acquire));
}

}  // namespace fuse::relight::bridge::ipc
