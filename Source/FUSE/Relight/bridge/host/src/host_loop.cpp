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
// Ported from dxvk-remix bridge/src/server/main.cpp@0867d3c (wWinMain command loop, OnClientExited)

// FUSE Relight RL-2.3: host command loop (see host_loop.hpp).

#include <fuse/relight/bridge/host/host_loop.hpp>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

namespace fuse::relight::bridge::host {

bool parseHostFault(const std::string& text, HostFault& fault, uint32_t& atPresent) {
    const size_t at = text.find('@');
    if (at == std::string::npos || at + 1 >= text.size()) {
        return false;
    }
    const std::string kind = text.substr(0, at);
    char* end = nullptr;
    const unsigned long n = std::strtoul(text.c_str() + at + 1, &end, 10);
    if (end == nullptr || *end != '\0' || n == 0 || n > 0xFFFFFFFFul) {
        return false;
    }
    if (kind == "crash") {
        fault = HostFault::Crash;
    } else if (kind == "hang") {
        fault = HostFault::Hang;
    } else if (kind == "exit") {
        fault = HostFault::Exit;
    } else {
        return false;
    }
    atPresent = static_cast<uint32_t>(n);
    return true;
}

namespace {

ipc::Result sendRaw(ipc::Session& s, uint16_t command, const std::vector<uint8_t>& bytes) {
    return s.sendWith(command, 0, 0, bytes.size(), ipc::kInfinite, [&](uint8_t* p) {
        if (!bytes.empty()) {
            std::memcpy(p, bytes.data(), bytes.size());
        }
    });
}

ipc::Result sendReply(ipc::Session& s, ReplyKind kind, uint32_t uid, int32_t hr, Response& resp) {
    switch (kind) {
    case ReplyKind::None:
        return ipc::Result::Success;
    case ReplyKind::Result: {
        schema::cmd::Reply_Result r;
        r.requestUid = uid;
        r.hresult = hr;
        return s.send(r);
    }
    case ReplyKind::Data: {
        // Chunked (the client appends chunks until offset + size == totalBytes).
        const size_t total = resp.payload.size();
        if (total > 0xFFFFFFFFu) {
            resp.payload.clear();
            hr = kHrOutOfMemory;
        }
        size_t offset = 0;
        do {
            const size_t n = std::min<size_t>(kReplyChunkBytes, resp.payload.size() - offset);
            schema::cmd::Reply_Data d;
            d.requestUid = uid;
            d.hresult = hr;
            d.totalBytes = static_cast<uint32_t>(resp.payload.size());
            d.offset = static_cast<uint32_t>(offset);
            d.data.assign(resp.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                          resp.payload.begin() + static_cast<std::ptrdiff_t>(offset + n));
            const ipc::Result r = s.send(d);
            if (r != ipc::Result::Success) {
                return r;
            }
            offset += n;
        } while (offset < resp.payload.size());
        return ipc::Result::Success;
    }
    default: {
        std::vector<uint8_t> bytes = resp.payload.empty() ? encodeDefaultReply(kind, hr) : std::move(resp.payload);
        patchReplyUid(bytes, uid);
        return sendRaw(s, replyCommand(kind), bytes);
    }
    }
}

}  // namespace

int runHostLoop(ipc::Session& session, ILocalBackend& backend, const HostLoopOptions& options, HostLoopStats* stats) {
    HostLoopStats local;
    HostLoopStats& st = stats != nullptr ? *stats : local;
    backend.setSharedHeap(session.hasHeap() ? &session.heap() : nullptr);
    for (;;) {
        ipc::Session::Incoming in;
        const ipc::Result r = session.receive(in, ipc::kInfinite);
        if (r == ipc::Result::PeerDead) {
            std::fprintf(stderr, "fuse-relight host: the client process exited unexpectedly, shutting down\n");
            return kHostExitClientDied;
        }
        if (r == ipc::Result::PeerClosed) {
            return kHostExitOk;
        }
        if (r != ipc::Result::Success) {
            std::fprintf(stderr, "fuse-relight host: receive failed (%s)\n", ipc::toString(r));
            return kHostExitProtocol;
        }
        const ipc::MessageHeader hdr = in.header;
        ++st.commands;
        if (hdr.command == static_cast<uint16_t>(schema::CommandId::Bridge_Terminate)) {
            session.release(in);
            return kHostExitOk;
        }
        if (isPresentCommand(hdr.command)) {
            ++st.presents;
            if (options.fault != HostFault::None && st.presents == options.faultAtPresent) {
                std::fprintf(stderr, "fuse-relight host: test fault at Present %llu\n",
                             static_cast<unsigned long long>(st.presents));
                std::fflush(stderr);
                switch (options.fault) {
                case HostFault::Crash:
                    if (options.crash) {
                        options.crash();
                    }
                    std::abort();
                case HostFault::Hang:
                    for (;;) {
                        ipc::sleepMs(1000);
                    }
                case HostFault::Exit:
                    session.release(in);
                    return kHostExitTestFault;
                case HostFault::None:
                    break;
                }
            }
        }
        const ReplyKind kind = replyKind(hdr.command, hdr.flags);
        Response resp;
        const int32_t hr = backend.execute(hdr.command, hdr.flags, hdr.handle, in.data, hdr.dataSize,
                                           kind != ReplyKind::None ? &resp : nullptr);
        session.release(in);
        if (kind != ReplyKind::None) {
            const ipc::Result sr = sendReply(session, kind, hdr.uid, hr, resp);
            if (sr == ipc::Result::PeerDead) {
                return kHostExitClientDied;
            }
            if (sr == ipc::Result::PeerClosed) {
                return kHostExitOk;
            }
            if (sr != ipc::Result::Success) {
                std::fprintf(stderr, "fuse-relight host: reply send failed (%s)\n", ipc::toString(sr));
                return kHostExitProtocol;
            }
            ++st.responses;
        }
        if (options.terminated && options.terminated()) {
            return kHostExitOk;
        }
    }
}

}  // namespace fuse::relight::bridge::host
