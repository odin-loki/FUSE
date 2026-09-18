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

/// Preflight for reconciling authoritative input against predicted local history (B7.4 deepen follow-up).
struct ReconcileInputPreflight {
    bool capacity_ok = false;
    bool buffer_empty = true;
    bool frame_in_window = false;
    bool has_retained_frame = false;
    bool has_prediction = false;

    [[nodiscard]] bool can_reconcile() const { return capacity_ok && frame_in_window; }
};

/// Preflight for reconciling confirmed remote input against rollback-buffer local prediction (B7.4 deepen follow-up).
struct ReconcileRollbackPreflight {
    bool capacity_ok = false;
    bool buffer_empty = true;
    bool frame_in_window = false;
    bool has_snapshot = false;
    bool has_local_prediction = false;

    [[nodiscard]] bool can_reconcile() const {
        return capacity_ok && !buffer_empty && frame_in_window && has_snapshot;
    }
};

/// True when `frame` is within the input history ring (or history is empty with non-zero capacity).
[[nodiscard]] bool can_reconcile_input_frame(const InputHistoryBuffer& history, u32 frame);

/// True when `frame` maps to a retained rollback-buffer snapshot within the ring window.
[[nodiscard]] bool can_reconcile_rollback_frame(const RollbackBuffer& buffer, u32 frame);

/// Inspect reconcile guards without mutating the input history ring (B7.4 deepen follow-up).
[[nodiscard]] ReconcileInputPreflight preflight_reconcile_input(const InputHistoryBuffer& history, u32 frame);

/// Inspect reconcile guards without mutating the rollback buffer (B7.4 deepen follow-up).
[[nodiscard]] ReconcileRollbackPreflight preflight_reconcile_rollback(const RollbackBuffer& buffer, u32 frame);

/// True when reconcile would return early without recording input (B7.4 deepen follow-up).
[[nodiscard]] bool should_skip_reconcile_input(const InputHistoryBuffer& history, u32 frame);

/// True when rollback reconcile would return early without recording remote input (B7.4 deepen follow-up).
[[nodiscard]] bool should_skip_reconcile_rollback(const RollbackBuffer& buffer, u32 frame);

/// Records authoritative input and compares it against the predicted local history (B7.4 stub).
[[nodiscard]] ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                                          const PlayerInput& authoritative);

/// Records confirmed remote input and compares it against the stored local prediction (B7.4 deepen).
[[nodiscard]] ReconcileResult reconcile_rollback_buffer(RollbackBuffer& buffer, u32 frame,
                                                          const PlayerInput& remote);

} // namespace fuse::net
