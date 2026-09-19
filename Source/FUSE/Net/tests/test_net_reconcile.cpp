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

    // --- reconcile preflight and empty-buffer helpers (B7.4 deepen follow-up) ---
    fuse::net::RollbackBuffer pristine_buffer;
    pristine_buffer.init(4);
    expectTrue(fuse::net::is_empty_rollback_buffer(pristine_buffer),
               "is_empty_rollback_buffer reports empty rollback ring");
    expectTrue(!fuse::net::is_empty_rollback_buffer(future_buffer),
               "is_empty_rollback_buffer rejects populated rollback ring");

    const fuse::net::RollbackReconcilePreflight retained_preflight =
        fuse::net::preflight_rollback_reconcile(future_buffer, 3u);
    expectTrue(!retained_preflight.buffer_empty, "rollback preflight sees populated buffer");
    expectTrue(retained_preflight.has_snapshot, "rollback preflight finds retained snapshot");
    expectTrue(retained_preflight.can_reconcile(), "rollback preflight accepts newest frame");
    expectTrue(!fuse::net::should_skip_rollback_reconcile(future_buffer, 3u),
               "should_skip accepts newest rollback frame");

    const fuse::net::RollbackReconcilePreflight future_preflight =
        fuse::net::preflight_rollback_reconcile(future_buffer, 9u);
    expectTrue(future_preflight.frame_beyond_newest, "rollback preflight marks future frame");
    expectTrue(fuse::net::should_skip_rollback_reconcile(future_buffer, 9u),
               "should_skip rejects future rollback frame");

    const fuse::net::RollbackReconcilePreflight evicted_preflight =
        fuse::net::preflight_rollback_reconcile(wrapped, 1u);
    expectTrue(evicted_preflight.frame_before_oldest, "rollback preflight marks evicted frame");
    expectTrue(fuse::net::should_skip_rollback_reconcile(wrapped, 1u),
               "should_skip rejects evicted rollback frame");

    fuse::net::InputHistoryBuffer pristine_history;
    pristine_history.init(8);
    const fuse::net::InputReconcilePreflight empty_history_preflight =
        fuse::net::preflight_input_reconcile(pristine_history, 2u);
    expectTrue(empty_history_preflight.history_empty, "input preflight marks empty history");
    expectTrue(empty_history_preflight.can_reconcile(), "empty history still allows reconcile window");
    expectTrue(!fuse::net::should_skip_input_reconcile(pristine_history, 2u),
               "should_skip allows reconcile on empty history with capacity");

    fuse::net::InputHistoryBuffer zero_capacity_history;
    zero_capacity_history.clear();
    const fuse::net::InputReconcilePreflight zero_preflight =
        fuse::net::preflight_input_reconcile(zero_capacity_history, 0u);
    expectTrue(zero_preflight.zero_capacity, "input preflight marks zero capacity");
    expectTrue(fuse::net::should_skip_input_reconcile(zero_capacity_history, 0u),
               "should_skip rejects zero-capacity history");
    // --- reconcile preflight helpers (B7.4 deepen follow-up) ---
    // --- preflight / empty-buffer / capacity guards (B7.4 deepen follow-up) ---
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

    zero_capacity_history.init(4);
    const fuse::net::InputReconcilePreflight zero_preflight = zero_capacity_history.preflight_reconcile(0u);
    expectTrue(zero_preflight.zero_capacity, "cleared history preflight reports zero_capacity");
    expectTrue(!zero_preflight.can_reconcile(), "zero capacity preflight cannot reconcile");
    expectTrue(zero_capacity_history.should_skip_reconcile(0u), "zero capacity should skip reconcile");

    fuse::net::RollbackBuffer rollback_preflight_buffer;
    rollback_preflight_buffer.init(4);
        fuse::net::GameSnapshot snap{};
        snap.frame = frame;
        rollback_preflight_buffer.store_snapshot(frame, snap);
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
        predicted.axis_lx = static_cast<std::int16_t>(frame);

    const fuse::net::ReconcileInputPreflight retained_preflight =
        fuse::net::preflight_reconcile_input(preflight_history, 4u);
    expectTrue(retained_preflight.capacity_ok, "input preflight sees non-zero capacity");
    expectTrue(!retained_preflight.buffer_empty, "input preflight sees populated ring");
    expectTrue(retained_preflight.has_retained_frame, "input preflight finds retained frame");
    expectTrue(retained_preflight.has_prediction, "input preflight sees local prediction");
    expectTrue(!fuse::net::should_skip_reconcile_input(preflight_history, 4u),
               "should_skip false for retained input frame");

    const fuse::net::ReconcileInputPreflight evicted_preflight =
        fuse::net::preflight_reconcile_input(preflight_history, 0u);
    expectTrue(!evicted_preflight.has_retained_frame, "input preflight has no retained evicted frame");
    expectTrue(fuse::net::should_skip_reconcile_input(preflight_history, 0u),
               "should_skip true for evicted input frame");

    empty_preflight_history.init(8);
    const fuse::net::ReconcileInputPreflight empty_preflight =
        fuse::net::preflight_reconcile_input(empty_preflight_history, 2u);
    expectTrue(empty_preflight.capacity_ok, "empty input history preflight has capacity");
    expectTrue(empty_preflight.buffer_empty, "empty input history preflight reports empty");
    expectTrue(empty_preflight.frame_in_window, "empty input history accepts in-window frame");
    expectTrue(!empty_preflight.has_retained_frame, "empty input history has no retained frame yet");
    expectTrue(!empty_preflight.has_prediction, "empty input history has no prediction");
    expectTrue(empty_preflight.can_reconcile(), "empty input history can reconcile new frame");
    expectTrue(!empty_preflight_history.should_skip_reconcile(2u),
               "buffer should_skip false for empty history with capacity");

    const fuse::net::ReconcileInputPreflight zero_preflight =
        fuse::net::preflight_reconcile_input(zero_capacity_history, 0u);
    expectTrue(!zero_preflight.capacity_ok, "cleared input history preflight has zero capacity");
    expectTrue(!zero_preflight.can_reconcile(), "zero-capacity input history cannot reconcile");
    expectTrue(zero_capacity_history.should_skip_reconcile(0u),
               "buffer should_skip true for zero-capacity history");

    fuse::net::RollbackBuffer preflight_buffer;
    preflight_buffer.init(4);
    for (fuse::u32 frame = 0; frame < 4; ++frame) {
        preflight_buffer.store_snapshot(frame, snap);

    fuse::net::PlayerInput preflight_local{};
    preflight_local.frame = 2;
    preflight_local.axis_lx = 100;
    preflight_buffer.store_local_input(2, preflight_local);

    const fuse::net::ReconcileRollbackPreflight rollback_preflight =
        fuse::net::preflight_reconcile_rollback(preflight_buffer, 2u);
    expectTrue(rollback_preflight.capacity_ok, "rollback preflight sees non-zero capacity");
    expectTrue(!rollback_preflight.buffer_empty, "rollback preflight sees populated buffer");
    expectTrue(rollback_preflight.frame_in_window, "rollback preflight accepts retained frame");
    expectTrue(rollback_preflight.has_snapshot, "rollback preflight finds snapshot");
    expectTrue(rollback_preflight.has_local_prediction, "rollback preflight sees local prediction");
    expectTrue(rollback_preflight.can_reconcile(), "rollback preflight can_reconcile for retained frame");
    expectTrue(!preflight_buffer.should_skip_reconcile(2u), "buffer should_skip false for retained rollback frame");

    const fuse::net::ReconcileRollbackPreflight rollback_future_preflight =
        fuse::net::preflight_reconcile_rollback(preflight_buffer, 9u);
    expectTrue(!rollback_future_preflight.frame_in_window, "rollback preflight rejects future frame");
    expectTrue(!rollback_future_preflight.has_snapshot, "rollback preflight has no future snapshot");
    expectTrue(!rollback_future_preflight.can_reconcile(), "rollback preflight cannot reconcile future frame");
    expectTrue(preflight_buffer.should_skip_reconcile(9u), "buffer should_skip true for future rollback frame");

    fuse::net::RollbackBuffer empty_preflight_buffer;
    empty_preflight_buffer.init(4);
    const fuse::net::ReconcileRollbackPreflight empty_rollback_preflight =
        fuse::net::preflight_reconcile_rollback(empty_preflight_buffer, 0u);
    expectTrue(empty_rollback_preflight.capacity_ok, "empty rollback buffer preflight has capacity");
    expectTrue(empty_rollback_preflight.buffer_empty, "empty rollback buffer preflight reports empty");
    expectTrue(!empty_rollback_preflight.frame_in_window, "empty rollback buffer rejects reconcile frame");
    expectTrue(!empty_rollback_preflight.can_reconcile(), "empty rollback buffer cannot reconcile");
    expectTrue(empty_preflight_buffer.should_skip_reconcile(0u),
               "buffer should_skip true for empty rollback buffer");

    fuse::net::RollbackBuffer zero_capacity_buffer;
    zero_capacity_buffer.clear();
    const fuse::net::ReconcileRollbackPreflight zero_rollback_preflight =
        fuse::net::preflight_reconcile_rollback(zero_capacity_buffer, 0u);
    expectTrue(!zero_rollback_preflight.capacity_ok, "cleared rollback buffer preflight has zero capacity");
    expectTrue(!zero_rollback_preflight.can_reconcile(), "zero-capacity rollback buffer cannot reconcile");
    expectTrue(fuse::net::should_skip_reconcile_rollback(zero_capacity_buffer, 0u),
               "should_skip true for zero-capacity rollback buffer");
    // --- reconcile preflight guards (B7.4 deepen follow-up) ---
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

    fuse::net::PlayerInput empty_frame_input{};
    empty_frame_input.frame = 1;
    const fuse::net::InputReconcilePreflight empty_preflight =
        fuse::net::preflight_reconcile_input(empty_preflight_history, 1, empty_frame_input);
    expectTrue(empty_preflight.history_empty, "preflight marks empty history");
    expectTrue(empty_preflight.can_reconcile(), "empty history still allows reconcile when in window");

    fuse::net::RollbackBuffer rollback_preflight;
    rollback_preflight.init(4);
    snap.frame = 3;
    rollback_preflight.store_snapshot(3, snap);
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

    zero_capacity_buffer.init(4);
    fuse::net::PlayerInput zero_remote{};
    zero_remote.frame = 0;
    const fuse::net::RollbackReconcilePreflight zero_preflight =
        fuse::net::preflight_reconcile_rollback(zero_capacity_buffer, 0, zero_remote);
    expectTrue(!zero_preflight.buffer_capacity_ok, "rollback preflight rejects zero capacity buffer");
    expectTrue(fuse::net::should_skip_rollback_reconcile(zero_capacity_buffer, 0, zero_remote),
               "should_skip_rollback_reconcile true on zero capacity buffer");
}

} // namespace fuse::net::tests
