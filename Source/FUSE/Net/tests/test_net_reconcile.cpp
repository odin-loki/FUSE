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

    // --- reconcile preflight helpers (B7.4 deepen follow-up) ---
    fuse::net::InputHistoryBuffer preflight_history;
    preflight_history.init(4);
    for (fuse::u32 frame = 0; frame < 5; ++frame) {
        fuse::net::PlayerInput predicted{};
        predicted.frame = frame;
        preflight_history.push_frame(frame, predicted);
    }

    const fuse::net::InputReconcilePreflight retained_preflight = preflight_history.preflight_reconcile(4u);
    expectTrue(!retained_preflight.zero_capacity, "input preflight sees non-zero capacity");
    expectTrue(!retained_preflight.ring_empty, "input preflight sees populated ring");
    expectTrue(retained_preflight.frame_in_window, "input preflight accepts retained frame");
    expectTrue(!retained_preflight.frame_evicted, "input preflight frame not evicted");
    expectTrue(!retained_preflight.frame_future, "input preflight frame not future");
    expectTrue(retained_preflight.has_prediction, "input preflight sees prediction on retained frame");
    expectTrue(retained_preflight.can_reconcile(), "input preflight can_reconcile for retained frame");
    expectTrue(!preflight_history.should_skip_reconcile(4u), "should_skip false for retained frame");
    expectTrue(fuse::net::can_reconcile_input(preflight_history, 4u),
               "can_reconcile_input matches preflight for retained frame");

    const fuse::net::InputReconcilePreflight evicted_preflight = preflight_history.preflight_reconcile(0u);
    expectTrue(evicted_preflight.frame_evicted, "input preflight marks evicted frame");
    expectTrue(!evicted_preflight.frame_in_window, "input preflight rejects evicted frame");
    expectTrue(!evicted_preflight.can_reconcile(), "input preflight cannot reconcile evicted frame");
    expectTrue(preflight_history.should_skip_reconcile(0u), "should_skip true for evicted frame");

    fuse::net::InputHistoryBuffer empty_preflight_history;
    empty_preflight_history.init(4);
    const fuse::net::InputReconcilePreflight empty_ring_preflight =
        empty_preflight_history.preflight_reconcile(2u);
    expectTrue(empty_ring_preflight.ring_empty, "empty input ring preflight reports ring_empty");
    expectTrue(empty_ring_preflight.frame_in_window, "empty input ring accepts any frame");
    expectTrue(empty_ring_preflight.can_reconcile(), "empty input ring can reconcile with capacity");
    expectTrue(!empty_preflight_history.should_skip_reconcile(2u), "empty ring should not skip reconcile");

    fuse::net::InputHistoryBuffer zero_capacity_history;
    zero_capacity_history.init(4);
    zero_capacity_history.clear();
    const fuse::net::InputReconcilePreflight zero_preflight = zero_capacity_history.preflight_reconcile(0u);
    expectTrue(zero_preflight.zero_capacity, "cleared history preflight reports zero_capacity");
    expectTrue(!zero_preflight.can_reconcile(), "zero capacity preflight cannot reconcile");
    expectTrue(zero_capacity_history.should_skip_reconcile(0u), "zero capacity should skip reconcile");

    fuse::net::RollbackBuffer rollback_preflight_buffer;
    rollback_preflight_buffer.init(4);
    for (fuse::u32 frame = 0; frame < 5; ++frame) {
        fuse::net::GameSnapshot snap{};
        snap.frame = frame;
        rollback_preflight_buffer.store_snapshot(frame, snap);
    }
    fuse::net::PlayerInput local_for_preflight{};
    local_for_preflight.frame = 4;
    rollback_preflight_buffer.store_local_input(4, local_for_preflight);

    const fuse::net::RollbackReconcilePreflight rollback_retained =
        rollback_preflight_buffer.preflight_reconcile(4u);
    expectTrue(!rollback_retained.zero_capacity, "rollback preflight sees non-zero capacity");
    expectTrue(!rollback_retained.ring_empty, "rollback preflight sees populated ring");
    expectTrue(rollback_retained.frame_in_window, "rollback preflight accepts retained frame");
    expectTrue(rollback_retained.has_snapshot, "rollback preflight sees retained snapshot");
    expectTrue(rollback_retained.has_local_input, "rollback preflight sees local input");
    expectTrue(rollback_retained.can_reconcile(), "rollback preflight can_reconcile for retained frame");
    expectTrue(!rollback_preflight_buffer.should_skip_reconcile(4u),
               "rollback should_skip false for retained frame");
    expectTrue(fuse::net::can_reconcile_rollback(rollback_preflight_buffer, 4u),
               "can_reconcile_rollback matches preflight for retained frame");

    const fuse::net::RollbackReconcilePreflight rollback_evicted =
        rollback_preflight_buffer.preflight_reconcile(0u);
    expectTrue(rollback_evicted.frame_evicted, "rollback preflight marks evicted frame");
    expectTrue(!rollback_evicted.can_reconcile(), "rollback preflight cannot reconcile evicted frame");
    expectTrue(rollback_preflight_buffer.should_skip_reconcile(0u),
               "rollback should_skip true for evicted frame");

    fuse::net::RollbackBuffer empty_rollback_preflight;
    empty_rollback_preflight.init(4);
    const fuse::net::RollbackReconcilePreflight empty_rollback_ring =
        empty_rollback_preflight.preflight_reconcile(0u);
    expectTrue(empty_rollback_ring.ring_empty, "empty rollback ring preflight reports ring_empty");
    expectTrue(!empty_rollback_ring.can_reconcile(), "empty rollback ring cannot reconcile");
    expectTrue(empty_rollback_preflight.should_skip_reconcile(0u),
               "empty rollback ring should skip reconcile");
}

} // namespace fuse::net::tests
