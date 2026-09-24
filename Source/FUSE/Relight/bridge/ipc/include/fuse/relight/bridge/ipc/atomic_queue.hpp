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
// Ported from dxvk-remix bridge/src/util/util_atomiccircularqueue.h@0867d3c

// FUSE Relight RL-2.1: single-producer single-consumer queue of fixed-size elements in shared
// memory, synchronized with atomics only.
//
// Semantics kept from upstream AtomicCircularQueue: SPSC only; the writer initializes the shared
// indices and the element array; a push on a full queue and a pull on an empty one wait (spin,
// yield, sleep) up to a timeout; "wait with an early-out signal"; isEmpty() for quiescent checks.
//
// Changes (revamp):
// - Monotonic 32-bit head/tail counters and a power-of-two capacity: all `capacity` slots are usable
//   (upstream sacrificed one) and fullness is exact. Upstream's index named `m_read` actually held
//   the write position; the names here are what they hold.
// - Acquire/release ordering instead of relaxed loads plus seq_cst fences.
// - Waits go through Waiter (wait.hpp): peer death/close are detected inside a blocked push/pull.
// - The reader validates `tail - head <= capacity` so a corrupted or hostile peer cannot make it
//   index out of range (Result::Malformed).
// - Layout is fixed-width and identical for x86 and x64 processes (static_asserts below).
#pragma once

#include <fuse/relight/bridge/ipc/result.hpp>
#include <fuse/relight/bridge/ipc/wait.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace fuse::relight::bridge::ipc {

// Shared header of both queue flavours. Each index sits on its own 128-byte line (upstream's
// kAlignment) so producer and consumer do not false-share. The semaphore slots are used by the
// blocking flavour on POSIX (IpcSemaphore::kSlotBytes each).
struct QueueControl {
    std::atomic<uint32_t> head;  // consumer position (monotonic)
    uint8_t pad0[124];
    std::atomic<uint32_t> tail;  // producer position (monotonic)
    uint8_t pad1[124];
    uint8_t semFree[64];
    uint8_t semFilled[64];
};
static_assert(sizeof(std::atomic<uint32_t>) == 4, "shared atomics must be plain 32-bit words");
static_assert(sizeof(QueueControl) == 384, "QueueControl layout must match across x86/x64");
static_assert(offsetof(QueueControl, tail) == 128, "QueueControl layout must match across x86/x64");

template <class T>
class AtomicQueue {
    static_assert(std::is_trivially_copyable_v<T>, "queue elements are copied through shared memory");

public:
    static size_t bytesFor(uint32_t capacity) noexcept { return sizeof(QueueControl) + size_t(capacity) * sizeof(T); }

    // Writer-side creation: constructs the shared indices and the element array.
    Result initialize(void* memory, uint32_t capacity) noexcept {
        if (!isPowerOfTwo(capacity)) {
            return Result::Malformed;
        }
        ctl_ = new (memory) QueueControl();
        ctl_->head.store(0, std::memory_order_relaxed);
        ctl_->tail.store(0, std::memory_order_relaxed);
        slots_ = new (static_cast<uint8_t*>(memory) + sizeof(QueueControl)) T[capacity];
        bind(capacity);
        return Result::Success;
    }

    Result attach(void* memory, uint32_t capacity) noexcept {
        if (!isPowerOfTwo(capacity)) {
            return Result::Malformed;
        }
        ctl_ = static_cast<QueueControl*>(memory);
        slots_ = reinterpret_cast<T*>(static_cast<uint8_t*>(memory) + sizeof(QueueControl));
        bind(capacity);
        return Result::Success;
    }

    // Producer. Waits while the queue is full (back-pressure).
    Result push(const T& value, uint32_t timeoutMs, const WaitContext* ctx) noexcept {
        const uint32_t t = ctl_->tail.load(std::memory_order_relaxed);
        Waiter waiter(ctx, timeoutMs);
        Result last = Result::Success;
        for (;;) {
            const uint32_t h = ctl_->head.load(std::memory_order_acquire);
            const uint32_t used = t - h;
            if (used > capacity_) {
                return Result::Malformed;
            }
            if (used < capacity_) {
                slots_[t & mask_] = value;
                ctl_->tail.store(t + 1, std::memory_order_release);
                return Result::Success;
            }
            if (last != Result::Success) {
                return last;
            }
            last = waiter.step();  // on a terminal result, re-check once before reporting it
        }
    }

    // Consumer. Waits while the queue is empty; queued elements are still delivered after the
    // peer closed or died.
    Result pull(T& out, uint32_t timeoutMs, const WaitContext* ctx) noexcept {
        Result r = peek(out, timeoutMs, ctx);
        if (r == Result::Success) {
            ctl_->head.store(ctl_->head.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        }
        return r;
    }

    // Consumer: copies the oldest element without removing it.
    Result peek(T& out, uint32_t timeoutMs, const WaitContext* ctx) const noexcept {
        const uint32_t h = ctl_->head.load(std::memory_order_relaxed);
        Waiter waiter(ctx, timeoutMs);
        Result last = Result::Success;
        for (;;) {
            const uint32_t t = ctl_->tail.load(std::memory_order_acquire);
            const uint32_t used = t - h;
            if (used > capacity_) {
                return Result::Malformed;
            }
            if (used != 0) {
                out = slots_[h & mask_];
                return Result::Success;
            }
            if (last != Result::Success) {
                return last;
            }
            last = waiter.step();  // on a terminal result, re-check once before reporting it
        }
    }

    // Exact only while one side is quiescent (as upstream isEmpty()).
    bool isEmpty() const noexcept {
        return ctl_->tail.load(std::memory_order_acquire) == ctl_->head.load(std::memory_order_acquire);
    }
    uint32_t size() const noexcept {
        return ctl_->tail.load(std::memory_order_acquire) - ctl_->head.load(std::memory_order_acquire);
    }
    uint32_t capacity() const noexcept { return capacity_; }

private:
    void bind(uint32_t capacity) noexcept {
        capacity_ = capacity;
        mask_ = capacity - 1;
    }

    QueueControl* ctl_ = nullptr;
    T* slots_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;
};

}  // namespace fuse::relight::bridge::ipc
