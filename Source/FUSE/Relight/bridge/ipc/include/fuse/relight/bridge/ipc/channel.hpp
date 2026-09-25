/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix bridge/src/util/{util_ipcchannel.h,util_bridgecommand.h,util_bridgecommand.cpp,util_common.h}@0867d3c

// FUSE Relight RL-2.1: one direction of the bridge — a command queue of message headers plus the
// data ring holding each message's payload.
//
// Semantics kept from upstream IpcChannel + Bridge::Command: a channel ties a command queue
// (Header: command id, flags, data position, object handle) to a data queue in one shared-memory
// block; the atomic or the blocking queue flavour can be chosen; each command carries a UID so
// responses can be matched to requests; payload bytes are written before the header is published.
//
// Changes (revamp):
// - The header also carries the payload size and the UID (upstream pushed the UID as the first
//   data word of every client command). The consumer resolves the payload from (offset, size) with
//   full validation instead of pulling words in lockstep and checking positions afterwards.
// - The flavour is a runtime property of the session (upstream: compile-time USE_BLOCKING_QUEUE).
// - No global "bridge running" flag or process-exit paths: every call returns a Result.
// - Header layout is fixed at 24 bytes for x86 and x64 (upstream Header was 12 bytes and relied on
//   identical struct packing of both compilers).
#pragma once

#include <fuse/relight/bridge/ipc/atomic_queue.hpp>
#include <fuse/relight/bridge/ipc/blocking_queue.hpp>
#include <fuse/relight/bridge/ipc/data_ring.hpp>
#include <fuse/relight/bridge/ipc/result.hpp>
#include <fuse/relight/bridge/ipc/wait.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace fuse::relight::bridge::ipc {

struct MessageHeader {
    uint16_t command = 0;   // schema::CommandId
    uint16_t flags = 0;     // MessageFlag bits
    uint32_t handle = 0;    // client-side object the call targets (upstream pHandle)
    uint32_t uid = 0;       // per-direction sequence number; responses echo the request's uid
    uint32_t dataOffset = 0;
    uint32_t dataSize = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(MessageHeader) == 24, "MessageHeader layout must match across x86/x64");

enum MessageFlag : uint16_t {
    kDataInSharedHeap = 1u << 0,  // the payload refers to shared-heap runs (upstream DataInSharedHeap)
};

enum class QueueKind : uint32_t { Atomic = 0, Blocking = 1 };

struct ChannelConfig {
    QueueKind queueKind = QueueKind::Atomic;
    uint32_t commandSlots = 4096;   // power of two
    uint32_t dataBytes = 4u << 20;  // power of two; a payload may use up to half
};

class Channel {
public:
    struct Outgoing {
        DataRing::Reservation reservation;
        uint8_t* data() const noexcept { return reservation.data; }
        uint32_t size() const noexcept { return reservation.size; }
    };
    struct Incoming {
        MessageHeader header;
        const uint8_t* data = nullptr;
        uint32_t end = 0;  // data-ring position to release
    };

    static size_t bytesFor(const ChannelConfig& config) noexcept;

    Result initialize(void* memory, const ChannelConfig& config, const std::string& name);
    Result attach(void* memory, const ChannelConfig& config, const std::string& name);

    // ---- producer (one thread at a time) -------------------------------------------------------
    // Reserves payload space (waits for the consumer: back-pressure). Write size() bytes at data().
    Result begin(uint32_t payloadBytes, uint32_t timeoutMs, const WaitContext* ctx, Outgoing& out) noexcept;
    // Publishes the header (waits for a free queue slot) and commits the payload. On failure the
    // reservation is dropped and nothing was sent.
    Result publish(const Outgoing& out, uint16_t command, uint16_t flags, uint32_t handle, uint32_t uid,
                   uint32_t timeoutMs, const WaitContext* ctx) noexcept;

    // ---- consumer (one thread at a time) -------------------------------------------------------
    Result receive(Incoming& in, uint32_t timeoutMs, const WaitContext* ctx) noexcept;
    // Returns the payload space of `in` (and everything before it) to the producer.
    void release(const Incoming& in) noexcept;

    uint32_t pending() const noexcept;
    const DataRing& ring() const noexcept { return ring_; }
    QueueKind queueKind() const noexcept { return kind_; }

private:
    static size_t queueBytes(const ChannelConfig& config) noexcept;

    QueueKind kind_ = QueueKind::Atomic;
    AtomicQueue<MessageHeader> atomic_;
    BlockingQueue<MessageHeader> blocking_;
    DataRing ring_;
};

}  // namespace fuse::relight::bridge::ipc
