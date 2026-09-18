#pragma once

#include <fuse/net/game_state.hpp>
#include <fuse/net/input_history.hpp>
#include <fuse/types.hpp>

namespace fuse::net {

class RollbackBuffer;

enum class ReconcileAction : u8 {
    NoOp,      ///< Frame not tracked or no local prediction yet.
    Confirmed, ///< Authoritative input matches the predicted local input.
    Mismatch,  ///< Authoritative input differs — caller should rollback/resimulate.
};

struct ReconcileResult {
    ReconcileAction action = ReconcileAction::NoOp;
    u32 frame = 0;
};

/// Preflight checks before reconciling authoritative input against predicted history (B7.4 deepen follow-up).
struct InputReconcilePreflight {
    bool zero_capacity = false;
    bool ring_empty = true;
    /// True when `frame` is within the retained ring window (or the ring is empty with capacity).
    bool frame_in_window = false;
    bool frame_evicted = false;
    bool frame_future = false;
    bool has_prediction = false;

    [[nodiscard]] bool can_reconcile() const { return !zero_capacity && frame_in_window; }
};

/// Preflight checks before reconciling remote input against rollback-buffer local prediction (B7.4 deepen follow-up).
struct RollbackReconcilePreflight {
    bool zero_capacity = false;
    bool ring_empty = true;
    bool frame_in_window = false;
    bool frame_evicted = false;
    bool frame_future = false;
    bool has_snapshot = false;
    bool has_local_input = false;

    [[nodiscard]] bool can_reconcile() const {
        return !zero_capacity && !ring_empty && frame_in_window && has_snapshot;
    }
};

/// True when `frame` is within the input history ring (or history is empty with non-zero capacity).
[[nodiscard]] bool can_reconcile_input_frame(const InputHistoryBuffer& history, u32 frame);

/// True when `frame` maps to a retained rollback-buffer snapshot within the ring window.
[[nodiscard]] bool can_reconcile_rollback_frame(const RollbackBuffer& buffer, u32 frame);

/// Preflight reconcile against predicted input history without mutating the ring (B7.4 deepen follow-up).
[[nodiscard]] InputReconcilePreflight preflight_reconcile_input(const InputHistoryBuffer& history, u32 frame);

/// Preflight reconcile against rollback-buffer local prediction without mutating the ring (B7.4 deepen follow-up).
[[nodiscard]] RollbackReconcilePreflight preflight_reconcile_rollback(const RollbackBuffer& buffer, u32 frame);

/// True when reconcile would be rejected before recording (zero capacity or out-of-window frame).
[[nodiscard]] bool should_skip_reconcile_input(const InputHistoryBuffer& history, u32 frame);

/// True when reconcile would be rejected before recording (empty ring, missing snapshot, or out-of-window frame).
[[nodiscard]] bool should_skip_reconcile_rollback(const RollbackBuffer& buffer, u32 frame);

/// Convenience guard — `preflight_reconcile_input(history, frame).can_reconcile()` (B7.4 deepen follow-up).
[[nodiscard]] bool can_reconcile_input(const InputHistoryBuffer& history, u32 frame);

/// Convenience guard — `preflight_reconcile_rollback(buffer, frame).can_reconcile()` (B7.4 deepen follow-up).
[[nodiscard]] bool can_reconcile_rollback(const RollbackBuffer& buffer, u32 frame);

/// Records authoritative input and compares it against the predicted local history (B7.4 stub).
[[nodiscard]] ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                                          const PlayerInput& authoritative);

/// Records confirmed remote input and compares it against the stored local prediction (B7.4 deepen).
[[nodiscard]] ReconcileResult reconcile_rollback_buffer(RollbackBuffer& buffer, u32 frame,
                                                          const PlayerInput& remote);

} // namespace fuse::net
