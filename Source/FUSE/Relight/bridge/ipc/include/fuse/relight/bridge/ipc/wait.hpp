// FUSE Relight RL-2.1: bounded waits with peer-death and peer-close detection.
// Copyright (c) 2026 FUSE contributors (MIT). New code: upstream waited with timeout x retries
// loops (util_bridgecommand.cpp waitForCommand, util_atomiccircularqueue.h) plus an OS exit
// callback that flipped a global "bridge running" flag. Here every blocking wait in the IPC core
// polls the peer through one Waiter, so every wait returns PeerDead within peerCheckMs of the
// peer's death and none depends on a global flag.
#pragma once

#include <fuse/relight/bridge/ipc/platform.hpp>
#include <fuse/relight/bridge/ipc/result.hpp>

#include <atomic>
#include <cstdint>

namespace fuse::relight::bridge::ipc {

// Session state words in the shared control block (one per side).
enum class PeerState : uint32_t { None = 0, Attached = 1, Running = 2, Closed = 3 };

struct WaitContext {
    PeerProcess* peer = nullptr;                       // polled for liveness when set
    const std::atomic<uint32_t>* peerState = nullptr;  // PeerState of the other side
    const std::atomic<bool>* cancel = nullptr;         // optional early out (upstream pbEarlyOutSignal)
    uint32_t peerCheckMs = 20;
};

class Waiter {
public:
    Waiter(const WaitContext* ctx, uint32_t timeoutMs) noexcept
        : ctx_(ctx), timeoutMs_(timeoutMs), start_(nowMs()), lastPeerCheck_(start_) {}

    // Checks the peer and the deadline without sleeping. Success means "keep waiting".
    Result poll(bool forcePeerCheck = false) noexcept {
        const uint64_t now = nowMs();
        if (ctx_ != nullptr) {
            if (ctx_->cancel != nullptr && ctx_->cancel->load(std::memory_order_relaxed)) {
                return Result::Timeout;
            }
            if (ctx_->peerState != nullptr &&
                ctx_->peerState->load(std::memory_order_acquire) == static_cast<uint32_t>(PeerState::Closed)) {
                return Result::PeerClosed;
            }
            if (ctx_->peer != nullptr && ctx_->peer->attached() &&
                (forcePeerCheck || now - lastPeerCheck_ >= ctx_->peerCheckMs)) {
                lastPeerCheck_ = now;
                if (!ctx_->peer->alive()) {
                    return Result::PeerDead;
                }
            }
        }
        if (timeoutMs_ != kInfinite && now - start_ >= timeoutMs_) {
            return Result::Timeout;
        }
        return Result::Success;
    }

    // Backs off (spin, then yield, then 1 ms sleeps) and polls. Success means "try again".
    Result step() noexcept {
        if (timeoutMs_ == kNoWait) {
            const Result r = poll(true);
            return r == Result::Success ? Result::Timeout : r;
        }
        ++spins_;
        if (spins_ < 64) {
            cpuRelax();
        } else if (spins_ < 256) {
            sleepMs(0);
        } else {
            sleepMs(1);
        }
        return poll();
    }

    // Length of the next OS-level wait slice (semaphore waits), never longer than the peer poll.
    uint32_t sliceMs() const noexcept {
        uint32_t slice = ctx_ != nullptr ? ctx_->peerCheckMs : 20;
        if (timeoutMs_ != kInfinite) {
            const uint64_t elapsed = nowMs() - start_;
            const uint64_t left = elapsed >= timeoutMs_ ? 0 : timeoutMs_ - elapsed;
            if (left < slice) {
                slice = static_cast<uint32_t>(left);
            }
        }
        return slice;
    }

    uint32_t timeoutMs() const noexcept { return timeoutMs_; }

private:
    const WaitContext* ctx_;
    uint32_t timeoutMs_;
    uint64_t start_;
    uint64_t lastPeerCheck_;
    uint32_t spins_ = 0;
};

}  // namespace fuse::relight::bridge::ipc
