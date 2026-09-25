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

// FUSE Relight RL-2.3: client end of the bridge with the crash/hang fallback (see link.hpp).

#include <fuse/relight/bridge/host/link.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fuse::relight::bridge::host {

const char* toString(FallbackReason r) noexcept {
    switch (r) {
    case FallbackReason::None: return "none";
    case FallbackReason::HostDied: return "host died";
    case FallbackReason::HostClosed: return "host closed the session";
    case FallbackReason::HostHung: return "host hung";
    case FallbackReason::SpawnFailed: return "host could not be started";
    case FallbackReason::HandshakeFailed: return "handshake failed";
    case FallbackReason::VersionMismatch: return "protocol or schema mismatch";
    case FallbackReason::ProtocolError: return "protocol error";
    case FallbackReason::Requested: return "requested";
    }
    return "?";
}

const char* toString(LinkMode m) noexcept {
    switch (m) {
    case LinkMode::Bridged: return "bridged";
    case LinkMode::Passthrough: return "passthrough";
    case LinkMode::Failed: return "failed";
    }
    return "?";
}

BridgeLink::BridgeLink(const LinkConfig& config, BackendFactory factory)
    : config_(config), factory_(std::move(factory)), journal_(config.journalLimitBytes) {}

BridgeLink::~BridgeLink() {
    shutdown();
    backend_.reset();
}

ipc::Result BridgeLink::launch(const HostLaunch& launch) {
    static std::atomic<uint32_t> counter {0};
    std::string name = launch.sessionName;
    if (name.empty()) {
        name = "fuse-relight-" + std::to_string(ipc::currentPid()) + "-" + std::to_string(counter.fetch_add(1));
    }
    std::unique_ptr<ipc::Session> s;
    ipc::Result r = ipc::Session::create(name, launch.session, s);
    if (r != ipc::Result::Success) {
        error_ = std::string("session create failed: ") + ipc::toString(r);
        fallBack(FallbackReason::SpawnFailed);
        return r;
    }
    auto child = std::make_shared<ipc::ChildProcess>();
    std::vector<std::string> args {launch.hostExe, kArgSession, name, "--client-pid", std::to_string(ipc::currentPid())};
    args.insert(args.end(), launch.extraArgs.begin(), launch.extraArgs.end());
    r = child->spawn(args);
    if (r != ipc::Result::Success) {
        error_ = "cannot start " + launch.hostExe;
        fallBack(FallbackReason::SpawnFailed);
        return r;
    }
    hostProcess_ = child;
    r = s->handshake(config_.startupTimeoutMs);
    if (r != ipc::Result::Success) {
        error_ = std::string("handshake failed: ") + ipc::toString(r);
        if (!s->peer().rejectReason.empty()) {
            error_ += " (" + s->peer().rejectReason + ")";
        }
        child->kill();
        fallBack(r == ipc::Result::VersionMismatch ? FallbackReason::VersionMismatch : FallbackReason::HandshakeFailed);
        return r;
    }
    attach(std::move(s), [child] { child->kill(); });
    return ipc::Result::Success;
}

void BridgeLink::attach(std::unique_ptr<ipc::Session> session, std::function<void()> killHost) {
    session_ = std::move(session);
    killHost_ = std::move(killHost);
    mode_ = LinkMode::Bridged;
}

FallbackReason BridgeLink::reasonFor(ipc::Result r) noexcept {
    switch (r) {
    case ipc::Result::PeerDead: return FallbackReason::HostDied;
    case ipc::Result::PeerClosed: return FallbackReason::HostClosed;
    case ipc::Result::Timeout: return FallbackReason::HostHung;
    case ipc::Result::VersionMismatch: return FallbackReason::VersionMismatch;
    default: return FallbackReason::ProtocolError;
    }
}

void BridgeLink::fallBack(FallbackReason reason) {
    if (mode_ != LinkMode::Bridged || inFallback_) {
        return;
    }
    inFallback_ = true;
    const uint64_t t0 = ipc::nowMs();
    if (killHost_) {
        killHost_();  // a hung host must not come back and present into the game's window
    }
    if (session_) {
        session_->close();
        session_.reset();
    }
    pendingPresents_.clear();
    reason_ = reason;
    ++fallbacks_;
    std::string err;
    backend_ = factory_ ? factory_(err) : nullptr;
    if (!backend_) {
        mode_ = LinkMode::Failed;
        error_ = "fallback backend unavailable: " + err;
    } else if (!journal_.enabled()) {
        mode_ = LinkMode::Failed;
        error_ = "fallback impossible: the command journal exceeded its limit";
    } else {
        journal_.replay([&](const JournalEntry& e) {
            backend_->execute(e.command, 0, e.handle, e.payload.data(), e.payload.size(), nullptr);
            ++replayed_;
        });
        journal_.clear();
        mode_ = LinkMode::Passthrough;
    }
    fallbackMs_ = ipc::nowMs() - t0;
    std::fprintf(stderr, "fuse-relight bridge: %s -> %s (%llu commands replayed in %llu ms)%s%s\n", toString(reason),
                 toString(mode_), static_cast<unsigned long long>(replayed_),
                 static_cast<unsigned long long>(fallbackMs_), error_.empty() ? "" : ": ", error_.c_str());
    inFallback_ = false;
    if (onFallback) {
        onFallback(reason);
    }
}

bool BridgeLink::heapPut(const void* p, size_t n, ipc::HeapRef& ref) {
    if (n > 0xFFFFFFFFu) {
        return false;
    }
    if (session_->heap().allocate(static_cast<uint32_t>(n), ref, config_.hangTimeoutMs) != ipc::Result::Success) {
        return false;  // sent inline instead (or fails there, which triggers the fallback)
    }
    std::memcpy(session_->heap().data(ref), p, n);
    return true;
}

ipc::Result BridgeLink::sendToHost(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size) {
    return session_->sendWith(command, flags, handle, size, config_.hangTimeoutMs, [&](uint8_t* p) {
        if (size != 0) {
            std::memcpy(p, data, size);
        }
    });
}

ipc::Result BridgeLink::awaitResponse(uint32_t uid, ReplyKind kind, Response* out) {
    uint32_t dataReceived = 0;
    bool dataStarted = false;
    for (;;) {
        ipc::Session::Incoming in;
        ipc::Result r = session_->receive(in, config_.hangTimeoutMs);
        if (r != ipc::Result::Success) {
            return r;
        }
        const uint16_t cmd = in.header.command;
        if (!isReplyCommand(cmd)) {
            // Host notifications (developer-menu input state, debug text) may arrive at any time.
            ipc::WireReader rd(in.data, in.header.dataSize);
            if (cmd == static_cast<uint16_t>(schema::CommandId::Bridge_InputState)) {
                schema::cmd::Bridge_InputState st;
                const bool ok = schema::cmd::decode(rd, st) && rd.atEnd();
                session_->release(in);
                if (!ok) {
                    return ipc::Result::Malformed;
                }
                if (onInputState) {
                    onInputState(st.uiActive != 0);
                }
                continue;
            }
            if (cmd == static_cast<uint16_t>(schema::CommandId::Bridge_DebugMessage)) {
                schema::cmd::Bridge_DebugMessage m;
                const bool ok = schema::cmd::decode(rd, m) && rd.atEnd();
                session_->release(in);
                if (!ok) {
                    return ipc::Result::Malformed;
                }
                if (onDebugMessage) {
                    onDebugMessage(m.level, m.text);
                }
                continue;
            }
            session_->release(in);
            return ipc::Result::Malformed;
        }
        if (in.header.dataSize < 8) {
            session_->release(in);
            return ipc::Result::Malformed;
        }
        uint32_t replyUid = 0;
        int32_t hr = 0;
        std::memcpy(&replyUid, in.data, 4);
        std::memcpy(&hr, in.data + 4, 4);
        if (replyUid != uid) {
            session_->release(in);
            auto it = std::find(pendingPresents_.begin(), pendingPresents_.end(), replyUid);
            if (it == pendingPresents_.end()) {
                return ipc::Result::Malformed;
            }
            pendingPresents_.erase(it);  // an asynchronous Present finished while we waited
            continue;
        }
        if (cmd != replyCommand(kind)) {
            session_->release(in);
            return ipc::Result::Malformed;
        }
        if (kind == ReplyKind::Data) {
            schema::cmd::Reply_Data d;
            ipc::WireReader rd(in.data, in.header.dataSize);
            const bool ok = schema::cmd::decode(rd, d) && rd.atEnd();
            session_->release(in);
            if (!ok || d.offset != dataReceived || size_t(d.offset) + d.data.size() > d.totalBytes) {
                return ipc::Result::Malformed;
            }
            if (out != nullptr) {
                if (!dataStarted) {
                    out->payload.assign(d.totalBytes, 0);
                }
                if (!d.data.empty()) {
                    std::memcpy(out->payload.data() + d.offset, d.data.data(), d.data.size());
                }
                out->result = d.hresult;
            }
            dataStarted = true;
            dataReceived += static_cast<uint32_t>(d.data.size());
            if (dataReceived >= d.totalBytes) {
                return ipc::Result::Success;
            }
            continue;
        }
        if (out != nullptr) {
            out->result = hr;
            if (kind == ReplyKind::Result) {
                out->payload.clear();
            } else {
                out->payload.assign(in.data, in.data + in.header.dataSize);
            }
        }
        session_->release(in);
        auto it = std::find(pendingPresents_.begin(), pendingPresents_.end(), uid);
        if (it != pendingPresents_.end()) {
            pendingPresents_.erase(it);
        }
        return ipc::Result::Success;
    }
}

int32_t BridgeLink::executeLocal(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size,
                                 Response* out) {
    return backend_->execute(command, flags, handle, data, size, out);
}

int32_t BridgeLink::callRaw(uint16_t command, uint16_t flags, uint32_t handle, const uint8_t* data, size_t size,
                            Response* out, const std::vector<uint8_t>* wire, const ipc::HeapRef* heap) {
    if (mode_ == LinkMode::Failed) {
        return kHrDeviceLost;
    }
    if (mode_ == LinkMode::Passthrough) {
        return executeLocal(command, flags, handle, data, size, out);
    }
    if (!session_) {
        fallBack(FallbackReason::ProtocolError);
        return mode_ == LinkMode::Passthrough ? executeLocal(command, flags, handle, data, size, out) : kHrDeviceLost;
    }
    const bool present = isPresentCommand(command);
    // Destroying a device completes its outstanding frames first (a D3D9 device finishes its queued
    // Presents before it goes away). This is also where a host that died on the last frames of a
    // run is noticed on the game's thread: shutdown() runs from DLL_PROCESS_DETACH and cannot
    // fall back there.
    if (!pendingPresents_.empty() && methodName(command) == "Destroy" &&
        interfaceName(command) == "IDirect3DDevice9Ex") {
        ipc::Result dr = ipc::Result::Success;
        while (dr == ipc::Result::Success && !pendingPresents_.empty()) {
            dr = awaitResponse(pendingPresents_.front(), ReplyKind::Result, nullptr);
        }
        if (dr != ipc::Result::Success) {
            fallBack(reasonFor(dr));
            return mode_ == LinkMode::Passthrough ? executeLocal(command, flags, handle, data, size, out) : kHrDeviceLost;
        }
    }
    uint16_t wireFlags = flags;
    if (out != nullptr || present) {
        wireFlags = static_cast<uint16_t>(wireFlags | kWantReply);
    }
    const ReplyKind kind = replyKind(command, wireFlags);
    const uint8_t* wd = wire != nullptr ? wire->data() : data;
    const size_t ws = wire != nullptr ? wire->size() : size;
    ipc::Result r = sendToHost(command, wireFlags, handle, wd, ws);
    if (r != ipc::Result::Success && heap != nullptr && session_) {
        session_->heap().freeLocal(*heap);
    }
    if (r == ipc::Result::Success) {
        const uint32_t uid = session_->lastSentUid();
        if (kind == ReplyKind::None) {
            journal_.append(command, handle, data, size);
            return kHrOk;
        }
        if (out == nullptr && present) {
            // Asynchronous Present: run ahead by at most maxFrameLatency frames.
            journal_.append(command, handle, data, size);
            pendingPresents_.push_back(uid);
            while (r == ipc::Result::Success && pendingPresents_.size() > config_.maxFrameLatency) {
                r = awaitResponse(pendingPresents_.front(), ReplyKind::Result, nullptr);
            }
            if (r == ipc::Result::Success) {
                return kHrOk;
            }
            fallBack(reasonFor(r));  // the frame is already in the journal's history: skip it
            return mode_ == LinkMode::Failed ? kHrDeviceLost : kHrOk;
        }
        Response local;
        Response* dst = out != nullptr ? out : &local;
        r = awaitResponse(uid, kind, dst);
        if (r == ipc::Result::Success) {
            journal_.append(command, handle, data, size);
            return dst->result;
        }
    }
    // The host failed before this command completed: rebuild the state in-process and run the
    // command there (it was not journaled, so it runs exactly once).
    fallBack(reasonFor(r));
    if (mode_ != LinkMode::Passthrough) {
        return kHrDeviceLost;
    }
    return executeLocal(command, flags, handle, data, size, out);
}

void BridgeLink::shutdown() {
    if (mode_ == LinkMode::Bridged && session_) {
        ipc::Result r = ipc::Result::Success;
        while (r == ipc::Result::Success && !pendingPresents_.empty()) {
            r = awaitResponse(pendingPresents_.front(), ReplyKind::Result, nullptr);
        }
        if (r == ipc::Result::Success) {
            session_->send(schema::cmd::Bridge_Terminate {}, 0, 0, config_.hangTimeoutMs);
        }
        session_->close();
        if (hostProcess_) {
            int code = 0;
            if (hostProcess_->wait(r == ipc::Result::Success ? 10000 : 0, &code) != ipc::Result::Success) {
                hostProcess_->kill();
            }
        }
        session_.reset();
    }
    pendingPresents_.clear();
}

}  // namespace fuse::relight::bridge::host
