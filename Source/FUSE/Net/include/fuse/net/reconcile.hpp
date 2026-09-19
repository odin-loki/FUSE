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

/// Preflight for reconciling authoritative input against local prediction (B7.4 deepen follow-up).
struct InputReconcilePreflight {
    bool zero_capacity = true;
    bool history_empty = true;
    bool frame_before_oldest = false;
    bool frame_beyond_newest = false;

    [[nodiscard]] bool can_reconcile() const {
        return !zero_capacity && !frame_before_oldest && !frame_beyond_newest;
    }

    [[nodiscard]] bool should_skip() const { return !can_reconcile(); }
};

/// Preflight for reconciling remote input against a rollback-buffer snapshot (B7.4 deepen follow-up).
struct RollbackReconcilePreflight {
    bool zero_capacity = true;
    bool buffer_empty = true;
    bool frame_before_oldest = false;
    bool frame_beyond_newest = false;
    bool has_snapshot = false;

    [[nodiscard]] bool can_reconcile() const {
        return !zero_capacity && !buffer_empty && !frame_before_oldest && !frame_beyond_newest && has_snapshot;
    }

    [[nodiscard]] bool should_skip() const { return !can_reconcile(); }
};

/// True when the rollback buffer ring has no retained snapshots (B7.4 deepen follow-up).
[[nodiscard]] bool is_empty_rollback_buffer(const RollbackBuffer& buffer);

/// Preflight reconcile window checks without mutating history (B7.4 deepen follow-up).
[[nodiscard]] InputReconcilePreflight preflight_input_reconcile(const InputHistoryBuffer& history, u32 frame);

/// True when reconcile should be skipped for `frame` (out of window or zero capacity).
[[nodiscard]] bool should_skip_input_reconcile(const InputHistoryBuffer& history, u32 frame);

/// Preflight rollback-buffer reconcile without recording remote input (B7.4 deepen follow-up).
[[nodiscard]] RollbackReconcilePreflight preflight_rollback_reconcile(const RollbackBuffer& buffer, u32 frame);

/// True when rollback reconcile should be skipped for `frame`.
[[nodiscard]] bool should_skip_rollback_reconcile(const RollbackBuffer& buffer, u32 frame);

/// True when `frame` is within the input history ring (or history is empty with non-zero capacity).
[[nodiscard]] bool can_reconcile_input_frame(const InputHistoryBuffer& history, u32 frame);

/// True when `frame` maps to a retained rollback-buffer snapshot within the ring window.
[[nodiscard]] bool can_reconcile_rollback_frame(const RollbackBuffer& buffer, u32 frame);

/// Records authoritative input and compares it against the predicted local history (B7.4 stub).
[[nodiscard]] ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                                          const PlayerInput& authoritative);

/// Records confirmed remote input and compares it against the stored local prediction (B7.4 deepen).
[[nodiscard]] ReconcileResult reconcile_rollback_buffer(RollbackBuffer& buffer, u32 frame,
                                                          const PlayerInput& remote);

} // namespace fuse::net
