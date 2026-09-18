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
    bool history_empty = true;
    bool buffer_capacity_ok = false;
    bool frame_in_window = false;
    /// True when `input.frame` matches the reconcile `frame` argument.
    bool input_frame_ok = true;
    bool has_prediction = false;

    [[nodiscard]] bool can_reconcile() const {
        return buffer_capacity_ok && frame_in_window && input_frame_ok;
    }
};

/// Preflight checks before reconciling remote input against rollback-buffer predictions (B7.4 deepen follow-up).
struct RollbackReconcilePreflight {
    bool buffer_empty = true;
    bool buffer_capacity_ok = false;
    bool frame_in_window = false;
    /// True when `remote.frame` matches the reconcile `frame` argument.
    bool input_frame_ok = true;
    bool has_snapshot = false;
    bool has_local_input = false;

    [[nodiscard]] bool can_reconcile() const {
        return buffer_capacity_ok && frame_in_window && input_frame_ok && has_snapshot;
    }
};

/// True when `input.frame` matches the reconcile `frame` argument.
[[nodiscard]] bool input_frame_matches(u32 frame, const PlayerInput& input);

/// True when `frame` is within the input history ring (or history is empty with non-zero capacity).
[[nodiscard]] bool can_reconcile_input_frame(const InputHistoryBuffer& history, u32 frame);

/// True when `frame` maps to a retained rollback-buffer snapshot within the ring window.
[[nodiscard]] bool can_reconcile_rollback_frame(const RollbackBuffer& buffer, u32 frame);

/// Preflight reconcile against predicted input history without mutating the ring (B7.4 deepen follow-up).
[[nodiscard]] InputReconcilePreflight preflight_reconcile_input(const InputHistoryBuffer& history, u32 frame,
                                                                  const PlayerInput& input);

/// Preflight reconcile against rollback-buffer predictions without mutating the ring (B7.4 deepen follow-up).
[[nodiscard]] RollbackReconcilePreflight preflight_reconcile_rollback(const RollbackBuffer& buffer, u32 frame,
                                                                         const PlayerInput& remote);

/// True when reconcile guards fail and the call would be a no-op (B7.4 deepen follow-up).
[[nodiscard]] bool should_skip_input_reconcile(const InputHistoryBuffer& history, u32 frame, const PlayerInput& input);

/// True when rollback reconcile guards fail and the call would be a no-op (B7.4 deepen follow-up).
[[nodiscard]] bool should_skip_rollback_reconcile(const RollbackBuffer& buffer, u32 frame, const PlayerInput& remote);

/// Records authoritative input and compares it against the predicted local history (B7.4 stub).
[[nodiscard]] ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                                          const PlayerInput& authoritative);

/// Records confirmed remote input and compares it against the stored local prediction (B7.4 deepen).
[[nodiscard]] ReconcileResult reconcile_rollback_buffer(RollbackBuffer& buffer, u32 frame,
                                                          const PlayerInput& remote);

} // namespace fuse::net
