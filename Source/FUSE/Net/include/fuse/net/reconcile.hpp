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
    bool buffer_empty = true;
    bool has_snapshot = false;

        return !zero_capacity && !buffer_empty && !frame_before_oldest && !frame_beyond_newest && has_snapshot;


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

/// Preflight checks before reconciling authoritative input against predicted history (B7.4 deepen follow-up).
    bool zero_capacity = false;
    bool ring_empty = true;
    /// True when `frame` is within the retained ring window (or the ring is empty with capacity).
    bool frame_in_window = false;
    bool frame_evicted = false;
    bool frame_future = false;
    bool has_prediction = false;

    [[nodiscard]] bool can_reconcile() const { return !zero_capacity && frame_in_window; }

/// Preflight checks before reconciling remote input against rollback-buffer local prediction (B7.4 deepen follow-up).
    bool has_local_input = false;

        return !zero_capacity && !ring_empty && frame_in_window && has_snapshot;
/// Preflight for reconciling authoritative input against predicted local history (B7.4 deepen follow-up).
struct ReconcileInputPreflight {
    bool capacity_ok = false;
    bool has_retained_frame = false;

    [[nodiscard]] bool can_reconcile() const { return capacity_ok && frame_in_window; }

/// Preflight for reconciling confirmed remote input against rollback-buffer local prediction (B7.4 deepen follow-up).
struct ReconcileRollbackPreflight {
    bool has_local_prediction = false;

        return capacity_ok && !buffer_empty && frame_in_window && has_snapshot;
    bool buffer_capacity_ok = false;
    /// True when `input.frame` matches the reconcile `frame` argument.
    bool input_frame_ok = true;

        return buffer_capacity_ok && frame_in_window && input_frame_ok;

/// Preflight checks before reconciling remote input against rollback-buffer predictions (B7.4 deepen follow-up).
    /// True when `remote.frame` matches the reconcile `frame` argument.

        return buffer_capacity_ok && frame_in_window && input_frame_ok && has_snapshot;

[[nodiscard]] bool input_frame_matches(u32 frame, const PlayerInput& input);

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

/// Inspect reconcile guards without mutating the input history ring (B7.4 deepen follow-up).
[[nodiscard]] ReconcileInputPreflight preflight_reconcile_input(const InputHistoryBuffer& history, u32 frame);

/// Inspect reconcile guards without mutating the rollback buffer (B7.4 deepen follow-up).
[[nodiscard]] ReconcileRollbackPreflight preflight_reconcile_rollback(const RollbackBuffer& buffer, u32 frame);

/// True when reconcile would return early without recording input (B7.4 deepen follow-up).

/// True when rollback reconcile would return early without recording remote input (B7.4 deepen follow-up).
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
