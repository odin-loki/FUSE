#pragma once

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::jobs {

/// Work-stealing policy stubs for B1.5 deepen (victim pick, half-queue batch, empty fallback).
struct WorkStealParams {
    /// Minimum victim depth before a steal attempt (owner keeps at least one job when size == 1).
    static constexpr u32 kMinVictimQueueSize = 1;
};

/// Pick the victim worker for steal attempt `round` (0-based), rotating from `(thief + 1)`.
/// When `workerCount <= 1`, returns `thief` (no alternate victims).
u32 pickStealVictim(u32 thief, u32 workerCount, u32 round);

/// Half-queue steal batch size stub: ceil(queueSize / 2), or 0 when empty.
u32 stealHalfQueueBatchSize(std::size_t victimQueueSize);

/// Empty-victim guard: false when the victim queue has nothing to steal.
bool canStealFromVictim(std::size_t victimQueueSize);

} // namespace fuse::jobs
