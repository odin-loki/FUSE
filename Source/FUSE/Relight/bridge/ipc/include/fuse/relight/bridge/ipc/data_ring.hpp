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
// Ported from dxvk-remix bridge/src/util/{util_circularbuffer,util_circularqueue}.h@0867d3c

// FUSE Relight RL-2.1: the data ring — variable-size payloads of one channel direction.
//
// Semantics kept from upstream CircularBuffer ("DataQueue"): payloads are written contiguously
// into a shared ring; a payload that does not fit before the end of the ring rolls over to offset 0
// (the tail is skipped); zero-copy writes ("begin_blob_push") are supported; the command header
// carries the payload's position so both sides can check they are in sync (Header::dataOffset).
//
// Changes (revamp):
// - Real back-pressure. Upstream tracked the reader's position with serverDataPos /
//   clientDataExpectedPos / serverResetPosRequired and a "data queue overwrite condition" path that
//   could still overwrite after retries or terminate the process. Here the reader publishes a
//   monotonic `readPos`; the writer waits (Waiter: timeout, peer death/close) until the payload fits.
// - A payload larger than maxPayload() (half the ring, so any position can take it after at most
//   one rollover) returns Result::TooLarge instead of exiting: the caller moves it to the shared heap.
// - The reader validates each (offset, size) against its own position and the capacity, so a
//   corrupt header cannot make it read outside the ring (Result::Malformed).
// - Byte-granular with 8-byte record alignment (upstream counted 32-bit words).
// - A reservation is committed only after its header was published, so a timed-out send leaves no
//   trace in the ring.
#pragma once

#include <fuse/relight/bridge/ipc/atomic_queue.hpp>
#include <fuse/relight/bridge/ipc/result.hpp>
#include <fuse/relight/bridge/ipc/wait.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace fuse::relight::bridge::ipc {

struct DataRingControl {
    std::atomic<uint32_t> readPos;   // consumer: everything before this may be overwritten
    uint8_t pad0[124];
    std::atomic<uint32_t> writePos;  // producer: diagnostics only (ordering comes from the headers)
    uint8_t pad1[124];
};
static_assert(sizeof(DataRingControl) == 256, "DataRingControl layout must match across x86/x64");

class DataRing {
public:
    static constexpr uint32_t kAlign = 8;

    struct Reservation {
        uint8_t* data = nullptr;
        uint32_t offset = 0;  // monotonic position of the payload's first byte
        uint32_t size = 0;    // payload bytes
        uint32_t end = 0;     // monotonic position after the (aligned) payload
    };

    static constexpr uint32_t alignUp(uint32_t v) noexcept { return (v + (kAlign - 1)) & ~(kAlign - 1); }
    static size_t bytesFor(uint32_t capacity) noexcept { return sizeof(DataRingControl) + capacity; }

    Result initialize(void* memory, uint32_t capacity) noexcept {
        if (!isPowerOfTwo(capacity) || capacity < 2 * kAlign) {
            return Result::Malformed;
        }
        ctl_ = new (memory) DataRingControl();
        ctl_->readPos.store(0, std::memory_order_relaxed);
        ctl_->writePos.store(0, std::memory_order_relaxed);
        return bind(memory, capacity);
    }

    Result attach(void* memory, uint32_t capacity) noexcept {
        if (!isPowerOfTwo(capacity) || capacity < 2 * kAlign) {
            return Result::Malformed;
        }
        ctl_ = static_cast<DataRingControl*>(memory);
        return bind(memory, capacity);
    }

    uint32_t capacity() const noexcept { return capacity_; }
    uint32_t maxPayload() const noexcept { return capacity_ / 2; }

    // ---- producer ----------------------------------------------------------------------------
    // Reserves `size` contiguous bytes, waiting for the reader to free space (back-pressure).
    Result reserve(uint32_t size, uint32_t timeoutMs, const WaitContext* ctx, Reservation& out) noexcept {
        if (size > maxPayload()) {
            return Result::TooLarge;
        }
        const uint32_t w = writeLocal_;
        const uint32_t aligned = alignUp(size);
        const uint32_t room = capacity_ - (w & mask_);
        const uint32_t skip = aligned > room ? room : 0;  // roll over to offset 0
        const uint32_t need = skip + aligned;
        Waiter waiter(ctx, timeoutMs);
        Result last = Result::Success;
        for (;;) {
            const uint32_t used = w - ctl_->readPos.load(std::memory_order_acquire);
            if (used > capacity_) {
                return Result::Malformed;
            }
            if (capacity_ - used >= need) {
                break;
            }
            if (last != Result::Success) {
                return last;
            }
            last = waiter.step();
        }
        out.offset = w + skip;
        out.size = size;
        out.end = w + need;
        out.data = data_ + (out.offset & mask_);
        return Result::Success;
    }

    // Makes the reservation permanent (call after the header that points at it was published).
    void commit(const Reservation& r) noexcept {
        writeLocal_ = r.end;
        ctl_->writePos.store(r.end, std::memory_order_release);
    }

    // ---- consumer ----------------------------------------------------------------------------
    // Resolves a payload announced by a header. Validates it against the consumer position.
    Result view(uint32_t offset, uint32_t size, const uint8_t*& out, uint32_t& end) const noexcept {
        const uint32_t ahead = offset - readLocal_;
        const uint32_t aligned = alignUp(size);
        if (size > maxPayload() || ahead > capacity_ || uint64_t(ahead) + aligned > capacity_ ||
            (offset & mask_) + uint64_t(aligned) > capacity_) {
            return Result::Malformed;
        }
        out = data_ + (offset & mask_);
        end = offset + aligned;
        return Result::Success;
    }

    // Frees everything up to `end` (returned by view) for the producer.
    void release(uint32_t end) noexcept {
        readLocal_ = end;
        ctl_->readPos.store(end, std::memory_order_release);
    }

    uint32_t used() const noexcept {
        return ctl_->writePos.load(std::memory_order_acquire) - ctl_->readPos.load(std::memory_order_acquire);
    }

private:
    Result bind(void* memory, uint32_t capacity) noexcept {
        data_ = static_cast<uint8_t*>(memory) + sizeof(DataRingControl);
        capacity_ = capacity;
        mask_ = capacity - 1;
        writeLocal_ = ctl_->writePos.load(std::memory_order_acquire);
        readLocal_ = ctl_->readPos.load(std::memory_order_acquire);
        return Result::Success;
    }

    DataRingControl* ctl_ = nullptr;
    uint8_t* data_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;
    uint32_t writeLocal_ = 0;  // producer-owned position
    uint32_t readLocal_ = 0;   // consumer-owned position
};

}  // namespace fuse::relight::bridge::ipc
