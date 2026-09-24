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
// Ported from dxvk-remix bridge/src/util/util_blockingcircularqueue.h@0867d3c

// FUSE Relight RL-2.1: single-producer single-consumer queue of fixed-size elements in shared
// memory, synchronized with two counting semaphores ("free slots" and "filled slots").
//
// Semantics kept from upstream BlockingCircularQueue: push waits on the free-slot semaphore and
// releases the filled one; pull does the reverse; waits have timeouts. Upstream measured it slower
// than the atomic queue and kept it behind USE_BLOCKING_QUEUE; FUSE keeps it selectable per session
// (QueueKind::Blocking) because it does not spin, which suits a host that idles for long periods.
//
// Changes (revamp):
// - Built on the same QueueControl header and monotonic counters as AtomicQueue (upstream derived
//   from the non-thread-safe CircularQueue and kept the position in process-local memory).
// - Semaphore waits are sliced (Waiter::sliceMs) so a blocked push/pull notices peer death/close.
// - Batching (begin_write_batch/end_write_batch) is dropped: the channel publishes one header per
//   message and the data ring carries the bulk, so a batch would save one semaphore post at most.
// - peek() is not offered (upstream's peek waited and re-released the semaphore); the session layer
//   never needs it.
// - POSIX: the semaphores are process-shared sem_t inside QueueControl; Win32: named semaphores.
#pragma once

#include <fuse/relight/bridge/ipc/atomic_queue.hpp>
#include <fuse/relight/bridge/ipc/platform.hpp>

#include <string>

namespace fuse::relight::bridge::ipc {

template <class T>
class BlockingQueue {
    static_assert(std::is_trivially_copyable_v<T>, "queue elements are copied through shared memory");

public:
    static size_t bytesFor(uint32_t capacity) noexcept { return sizeof(QueueControl) + size_t(capacity) * sizeof(T); }

    Result initialize(void* memory, uint32_t capacity, const std::string& name) {
        if (!isPowerOfTwo(capacity)) {
            return Result::Malformed;
        }
        ctl_ = new (memory) QueueControl();
        ctl_->head.store(0, std::memory_order_relaxed);
        ctl_->tail.store(0, std::memory_order_relaxed);
        slots_ = new (static_cast<uint8_t*>(memory) + sizeof(QueueControl)) T[capacity];
        capacity_ = capacity;
        mask_ = capacity - 1;
        Result r = free_.create(ctl_->semFree, name + ".free", capacity, capacity);
        if (r == Result::Success) {
            r = filled_.create(ctl_->semFilled, name + ".filled", 0, capacity);
        }
        return r;
    }

    Result attach(void* memory, uint32_t capacity, const std::string& name) {
        if (!isPowerOfTwo(capacity)) {
            return Result::Malformed;
        }
        ctl_ = static_cast<QueueControl*>(memory);
        slots_ = reinterpret_cast<T*>(static_cast<uint8_t*>(memory) + sizeof(QueueControl));
        capacity_ = capacity;
        mask_ = capacity - 1;
        Result r = free_.open(ctl_->semFree, name + ".free");
        if (r == Result::Success) {
            r = filled_.open(ctl_->semFilled, name + ".filled");
        }
        return r;
    }

    Result push(const T& value, uint32_t timeoutMs, const WaitContext* ctx) noexcept {
        Result r = acquire(free_, timeoutMs, ctx);
        if (r != Result::Success) {
            return r;
        }
        const uint32_t t = ctl_->tail.load(std::memory_order_relaxed);
        slots_[t & mask_] = value;
        ctl_->tail.store(t + 1, std::memory_order_release);
        filled_.post();
        return Result::Success;
    }

    Result pull(T& out, uint32_t timeoutMs, const WaitContext* ctx) noexcept {
        Result r = acquire(filled_, timeoutMs, ctx);
        if (r != Result::Success) {
            return r;
        }
        const uint32_t h = ctl_->head.load(std::memory_order_relaxed);
        const uint32_t t = ctl_->tail.load(std::memory_order_acquire);
        if (t - h == 0 || t - h > capacity_) {
            return Result::Malformed;
        }
        out = slots_[h & mask_];
        ctl_->head.store(h + 1, std::memory_order_release);
        free_.post();
        return Result::Success;
    }

    bool isEmpty() const noexcept {
        return ctl_->tail.load(std::memory_order_acquire) == ctl_->head.load(std::memory_order_acquire);
    }
    uint32_t size() const noexcept {
        return ctl_->tail.load(std::memory_order_acquire) - ctl_->head.load(std::memory_order_acquire);
    }
    uint32_t capacity() const noexcept { return capacity_; }

private:
    static Result acquire(IpcSemaphore& sem, uint32_t timeoutMs, const WaitContext* ctx) noexcept {
        Waiter waiter(ctx, timeoutMs);
        uint32_t slice = 0;
        for (;;) {
            Result r = sem.wait(slice);
            if (r != Result::Timeout) {
                return r;
            }
            r = waiter.poll(true);  // Timeout (kNoWait or deadline), PeerDead, PeerClosed or keep going
            if (r != Result::Success) {
                // Last chance: an element posted just before the peer closed is still delivered.
                return sem.wait(0) == Result::Success ? Result::Success : r;
            }
            slice = waiter.sliceMs();
        }
    }

    QueueControl* ctl_ = nullptr;
    T* slots_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;
    IpcSemaphore free_;
    IpcSemaphore filled_;
};

}  // namespace fuse::relight::bridge::ipc
