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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix bridge/src/util/{util_ipcchannel.h,util_bridgecommand.h,util_bridgecommand.cpp,util_common.h}@0867d3c

#include <fuse/relight/bridge/ipc/channel.hpp>

namespace fuse::relight::bridge::ipc {

const char* toString(Result r) noexcept {
    switch (r) {
    case Result::Success: return "Success";
    case Result::Timeout: return "Timeout";
    case Result::Failure: return "Failure";
    case Result::PeerDead: return "PeerDead";
    case Result::PeerClosed: return "PeerClosed";
    case Result::VersionMismatch: return "VersionMismatch";
    case Result::TooLarge: return "TooLarge";
    case Result::Exists: return "Exists";
    case Result::NotFound: return "NotFound";
    case Result::Malformed: return "Malformed";
    }
    return "Unknown";
}

namespace {
constexpr size_t align256(size_t v) noexcept { return (v + 255) & ~size_t(255); }
}  // namespace

size_t Channel::queueBytes(const ChannelConfig& config) noexcept {
    return align256(AtomicQueue<MessageHeader>::bytesFor(config.commandSlots));
}

size_t Channel::bytesFor(const ChannelConfig& config) noexcept {
    return queueBytes(config) + align256(DataRing::bytesFor(config.dataBytes));
}

Result Channel::initialize(void* memory, const ChannelConfig& config, const std::string& name) {
    kind_ = config.queueKind;
    auto* base = static_cast<uint8_t*>(memory);
    Result r = kind_ == QueueKind::Atomic ? atomic_.initialize(base, config.commandSlots)
                                          : blocking_.initialize(base, config.commandSlots, name);
    if (r != Result::Success) {
        return r;
    }
    return ring_.initialize(base + queueBytes(config), config.dataBytes);
}

Result Channel::attach(void* memory, const ChannelConfig& config, const std::string& name) {
    kind_ = config.queueKind;
    auto* base = static_cast<uint8_t*>(memory);
    Result r = kind_ == QueueKind::Atomic ? atomic_.attach(base, config.commandSlots)
                                          : blocking_.attach(base, config.commandSlots, name);
    if (r != Result::Success) {
        return r;
    }
    return ring_.attach(base + queueBytes(config), config.dataBytes);
}

Result Channel::begin(uint32_t payloadBytes, uint32_t timeoutMs, const WaitContext* ctx, Outgoing& out) noexcept {
    return ring_.reserve(payloadBytes, timeoutMs, ctx, out.reservation);
}

Result Channel::publish(const Outgoing& out, uint16_t command, uint16_t flags, uint32_t handle, uint32_t uid,
                        uint32_t timeoutMs, const WaitContext* ctx) noexcept {
    MessageHeader h;
    h.command = command;
    h.flags = flags;
    h.handle = handle;
    h.uid = uid;
    h.dataOffset = out.reservation.offset;
    h.dataSize = out.reservation.size;
    // The payload stores happen-before the header's release store in push(); the ring position is
    // committed only once the header is visible, so a failed push leaves the ring untouched.
    const Result r = kind_ == QueueKind::Atomic ? atomic_.push(h, timeoutMs, ctx) : blocking_.push(h, timeoutMs, ctx);
    if (r == Result::Success) {
        ring_.commit(out.reservation);
    }
    return r;
}

Result Channel::receive(Incoming& in, uint32_t timeoutMs, const WaitContext* ctx) noexcept {
    Result r = kind_ == QueueKind::Atomic ? atomic_.pull(in.header, timeoutMs, ctx)
                                          : blocking_.pull(in.header, timeoutMs, ctx);
    if (r != Result::Success) {
        return r;
    }
    return ring_.view(in.header.dataOffset, in.header.dataSize, in.data, in.end);
}

void Channel::release(const Incoming& in) noexcept { ring_.release(in.end); }

uint32_t Channel::pending() const noexcept { return kind_ == QueueKind::Atomic ? atomic_.size() : blocking_.size(); }

}  // namespace fuse::relight::bridge::ipc
