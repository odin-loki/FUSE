#include <fuse/net/input_history.hpp>
#include <fuse/net/reconcile.hpp>
#include <fuse/net/rollback_buffer.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_reconcile_tests() {
    fuse::net::InputHistoryBuffer history;
    history.init(32);

    fuse::net::PlayerInput predicted{};
    predicted.frame = 7;
    predicted.axis_lx = 500;
    history.store_predicted(7, predicted);

    fuse::net::PlayerInput authoritative = predicted;
    const fuse::net::ReconcileResult confirmed =
        fuse::net::reconcile_predicted_input(history, 7, authoritative);
    expectTrue(confirmed.action == fuse::net::ReconcileAction::Confirmed, "matching input confirmed");
    expectTrue(history.prediction_matches(7), "history records confirmed match");

    authoritative.axis_lx = 900;
    const fuse::net::ReconcileResult mismatch =
        fuse::net::reconcile_predicted_input(history, 7, authoritative);
    expectTrue(mismatch.action == fuse::net::ReconcileAction::Mismatch, "divergent input flagged");
    expectTrue(!history.prediction_matches(7), "history records mismatch after reconcile");

    fuse::net::InputHistoryBuffer empty_history;
    empty_history.init(8);
    fuse::net::PlayerInput remote_only{};
    remote_only.frame = 2;
    const fuse::net::ReconcileResult noop =
        fuse::net::reconcile_predicted_input(empty_history, 2, remote_only);
    expectTrue(noop.action == fuse::net::ReconcileAction::NoOp, "no prediction yields NoOp");
    expectTrue(empty_history.has_confirmed(2), "authoritative still recorded");

    fuse::net::InputHistoryBuffer buffer_method;
    buffer_method.init(16);
    fuse::net::PlayerInput local{};
    local.frame = 11;
    local.axis_ly = 250;
    buffer_method.push_frame(11, local);

    fuse::net::PlayerInput authority = local;
    const fuse::net::ReconcileResult via_buffer = buffer_method.reconcile_authoritative(11, authority);
    expectTrue(via_buffer.action == fuse::net::ReconcileAction::Confirmed,
               "buffer reconcile_authoritative confirms match");

    authority.axis_ly = 999;
    const fuse::net::ReconcileResult via_buffer_mismatch =
        buffer_method.reconcile_authoritative(11, authority);
    expectTrue(via_buffer_mismatch.action == fuse::net::ReconcileAction::Mismatch,
               "buffer reconcile_authoritative detects mismatch");
    expectTrue(!buffer_method.prediction_matches(11), "mismatch leaves prediction divergent");

    fuse::net::RollbackBuffer rollback_buffer;
    rollback_buffer.init(8);

    fuse::net::GameSnapshot snapshot{};
    snapshot.frame = 5;
    snapshot.checksum = 55;
    rollback_buffer.store_snapshot(5, snapshot);

    fuse::net::PlayerInput rollback_local{};
    rollback_local.frame = 5;
    rollback_local.axis_lx = 300;
    rollback_buffer.store_local_input(5, rollback_local);

    fuse::net::PlayerInput remote = rollback_local;
    const fuse::net::ReconcileResult buffer_confirmed =
        fuse::net::reconcile_rollback_buffer(rollback_buffer, 5, remote);
    expectTrue(buffer_confirmed.action == fuse::net::ReconcileAction::Confirmed,
               "rollback buffer reconcile confirms matching remote input");
    expectTrue(rollback_buffer.inputs_match(5), "rollback buffer inputs_match after confirm");

    remote.axis_lx = 999;
    const fuse::net::ReconcileResult buffer_mismatch =
        fuse::net::reconcile_rollback_buffer(rollback_buffer, 5, remote);
    expectTrue(buffer_mismatch.action == fuse::net::ReconcileAction::Mismatch,
               "rollback buffer reconcile flags mismatch");
    expectTrue(!rollback_buffer.inputs_match(5), "rollback buffer inputs_match false after mismatch");

    fuse::net::RollbackBuffer empty_buffer;
    empty_buffer.init(4);
    fuse::net::GameSnapshot only_snapshot{};
    only_snapshot.frame = 2;
    empty_buffer.store_snapshot(2, only_snapshot);
    fuse::net::PlayerInput remote_without_local{};
    remote_without_local.frame = 2;
    const fuse::net::ReconcileResult buffer_no_local =
        empty_buffer.reconcile_remote_input(2, remote_without_local);
    expectTrue(buffer_no_local.action == fuse::net::ReconcileAction::NoOp,
               "rollback buffer reconcile without local prediction is NoOp");
    expectTrue(empty_buffer.remote_confirmed(2), "remote input still recorded without local prediction");

    fuse::net::RollbackBuffer wrapped;
    wrapped.init(4);
    for (fuse::u32 frame = 0; frame < 6; ++frame) {
        fuse::net::GameSnapshot snap{};
        snap.frame = frame;
        snap.checksum = frame;
        wrapped.store_snapshot(frame, snap);
    }
    fuse::net::PlayerInput stale_remote{};
    stale_remote.frame = 1;
    const fuse::net::ReconcileResult evicted_buffer =
        fuse::net::reconcile_rollback_buffer(wrapped, 1, stale_remote);
    expectTrue(evicted_buffer.action == fuse::net::ReconcileAction::NoOp,
               "rollback buffer reconcile on evicted frame returns NoOp");
    expectTrue(!wrapped.remote_confirmed(1), "evicted frame remote input is not recorded");

    fuse::net::RollbackBuffer future_buffer;
    future_buffer.init(4);
    for (fuse::u32 frame = 0; frame < 4; ++frame) {
        fuse::net::GameSnapshot snap{};
        snap.frame = frame;
        future_buffer.store_snapshot(frame, snap);
    }
    fuse::net::PlayerInput future_remote{};
    future_remote.frame = 9;
    const fuse::net::ReconcileResult future_result =
        fuse::net::reconcile_rollback_buffer(future_buffer, 9, future_remote);
    expectTrue(future_result.action == fuse::net::ReconcileAction::NoOp,
               "rollback buffer reconcile rejects future frame beyond newest");
    expectTrue(!future_buffer.remote_confirmed(9), "future frame remote input is not recorded");
    expectTrue(fuse::net::can_reconcile_rollback_frame(future_buffer, 3u),
               "can_reconcile accepts newest retained rollback frame");
    expectTrue(!fuse::net::can_reconcile_rollback_frame(future_buffer, 9u),
               "can_reconcile rejects future rollback frame");

    fuse::net::InputHistoryBuffer future_history;
    future_history.init(4);
    for (fuse::u32 frame = 0; frame < 4; ++frame) {
        fuse::net::PlayerInput predicted{};
        predicted.frame = frame;
        future_history.push_frame(frame, predicted);
    }
    fuse::net::PlayerInput future_authority{};
    future_authority.frame = 8;
    const fuse::net::ReconcileResult future_history_result =
        fuse::net::reconcile_predicted_input(future_history, 8, future_authority);
    expectTrue(future_history_result.action == fuse::net::ReconcileAction::NoOp,
               "input history reconcile rejects future frame beyond newest");
    expectTrue(!future_history.has_confirmed(8u), "future frame is not stored by reconcile guard");

    // --- reconcile preflight guards (B7.4 deepen follow-up) ---
    fuse::net::InputHistoryBuffer preflight_history;
    preflight_history.init(8);
    fuse::net::PlayerInput preflight_predicted{};
    preflight_predicted.frame = 4;
    preflight_predicted.axis_lx = 100;
    preflight_history.store_predicted(4, preflight_predicted);

    fuse::net::PlayerInput matching{};
    matching.frame = 4;
    matching.axis_lx = 100;
    const fuse::net::InputReconcilePreflight ready_preflight =
        fuse::net::preflight_reconcile_input(preflight_history, 4, matching);
    expectTrue(ready_preflight.buffer_capacity_ok, "preflight sees input history capacity");
    expectTrue(ready_preflight.frame_in_window, "preflight accepts retained frame");
    expectTrue(ready_preflight.input_frame_ok, "preflight accepts matching input frame");
    expectTrue(ready_preflight.has_prediction, "preflight sees stored prediction");
    expectTrue(ready_preflight.can_reconcile(), "preflight can_reconcile for valid frame");

    fuse::net::PlayerInput mismatched{};
    mismatched.frame = 99;
    mismatched.axis_lx = 100;
    const fuse::net::InputReconcilePreflight mismatch_preflight =
        fuse::net::preflight_reconcile_input(preflight_history, 4, mismatched);
    expectTrue(!mismatch_preflight.input_frame_ok, "preflight rejects input frame mismatch");
    expectTrue(!mismatch_preflight.can_reconcile(), "preflight can_reconcile false on frame mismatch");
    expectTrue(fuse::net::should_skip_input_reconcile(preflight_history, 4, mismatched),
               "should_skip_input_reconcile true on frame mismatch");

    const fuse::net::ReconcileResult mismatch_result =
        fuse::net::reconcile_predicted_input(preflight_history, 4, mismatched);
    expectTrue(mismatch_result.action == fuse::net::ReconcileAction::NoOp,
               "frame mismatch reconcile returns NoOp");
    expectTrue(!preflight_history.has_confirmed(4u), "frame mismatch reconcile has no side effects");

    fuse::net::InputHistoryBuffer empty_preflight_history;
    empty_preflight_history.init(8);
    fuse::net::PlayerInput empty_frame_input{};
    empty_frame_input.frame = 1;
    const fuse::net::InputReconcilePreflight empty_preflight =
        fuse::net::preflight_reconcile_input(empty_preflight_history, 1, empty_frame_input);
    expectTrue(empty_preflight.history_empty, "preflight marks empty history");
    expectTrue(empty_preflight.can_reconcile(), "empty history still allows reconcile when in window");

    fuse::net::RollbackBuffer rollback_preflight;
    rollback_preflight.init(4);
    fuse::net::GameSnapshot snap{};
    snap.frame = 3;
    rollback_preflight.store_snapshot(3, snap);
    fuse::net::PlayerInput preflight_local{};
    preflight_local.frame = 3;
    preflight_local.axis_lx = 50;
    rollback_preflight.store_local_input(3, preflight_local);

    fuse::net::PlayerInput rollback_remote = preflight_local;
    const fuse::net::RollbackReconcilePreflight rollback_ready =
        fuse::net::preflight_reconcile_rollback(rollback_preflight, 3, rollback_remote);
    expectTrue(rollback_ready.buffer_capacity_ok, "rollback preflight sees buffer capacity");
    expectTrue(!rollback_ready.buffer_empty, "rollback preflight sees populated buffer");
    expectTrue(rollback_ready.has_snapshot, "rollback preflight sees retained snapshot");
    expectTrue(rollback_ready.has_local_input, "rollback preflight sees local prediction");
    expectTrue(rollback_ready.can_reconcile(), "rollback preflight can_reconcile for valid frame");

    fuse::net::PlayerInput rollback_mismatch = preflight_local;
    rollback_mismatch.frame = 7;
    const fuse::net::RollbackReconcilePreflight rollback_bad_frame =
        fuse::net::preflight_reconcile_rollback(rollback_preflight, 3, rollback_mismatch);
    expectTrue(!rollback_bad_frame.input_frame_ok, "rollback preflight rejects input frame mismatch");
    expectTrue(fuse::net::should_skip_rollback_reconcile(rollback_preflight, 3, rollback_mismatch),
               "should_skip_rollback_reconcile true on frame mismatch");

    const fuse::net::ReconcileResult rollback_mismatch_result =
        fuse::net::reconcile_rollback_buffer(rollback_preflight, 3, rollback_mismatch);
    expectTrue(rollback_mismatch_result.action == fuse::net::ReconcileAction::NoOp,
               "rollback frame mismatch reconcile returns NoOp");
    expectTrue(!rollback_preflight.remote_confirmed(3u),
               "rollback frame mismatch reconcile has no side effects");

    fuse::net::RollbackBuffer zero_capacity_buffer;
    zero_capacity_buffer.init(4);
    zero_capacity_buffer.clear();
    fuse::net::PlayerInput zero_remote{};
    zero_remote.frame = 0;
    const fuse::net::RollbackReconcilePreflight zero_preflight =
        fuse::net::preflight_reconcile_rollback(zero_capacity_buffer, 0, zero_remote);
    expectTrue(!zero_preflight.buffer_capacity_ok, "rollback preflight rejects zero capacity buffer");
    expectTrue(fuse::net::should_skip_rollback_reconcile(zero_capacity_buffer, 0, zero_remote),
               "should_skip_rollback_reconcile true on zero capacity buffer");
}

} // namespace fuse::net::tests
