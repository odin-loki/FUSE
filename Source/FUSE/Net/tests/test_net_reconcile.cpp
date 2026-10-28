#include <fuse/net/input_history.hpp>
#include <fuse/net/prediction_error.hpp>
#include <fuse/net/reconcile.hpp>
#include <fuse/net/reconcile_err.hpp>
#include <fuse/net/rollback_buffer.hpp>

#include "test_helpers.hpp"

#include <limits>

namespace fuse::net::tests {

void testPredictionError() {
    expectTrue(fuse::net::prediction_error(3.f, 1.f) == 2.f, "prediction_error is the absolute difference");
    expectTrue(fuse::net::prediction_error(1.f, 3.f) == 2.f, "prediction_error ignores operand order");
    expectTrue(fuse::net::prediction_error(std::numeric_limits<float>::quiet_NaN(), 1.f) == 0.f,
               "non-finite predicted returns 0");
    expectTrue(fuse::net::prediction_error(1.f, std::numeric_limits<float>::infinity()) == 0.f,
               "non-finite authoritative returns 0");

    expectTrue(fuse::net::needs_correction(0.2f, 0.1f), "needs_correction when error exceeds threshold");
    expectTrue(!fuse::net::needs_correction(0.05f, 0.1f), "needs_correction is false below threshold");
    expectTrue(!fuse::net::needs_correction(0.1f, 0.1f), "needs_correction is false when error equals threshold");
    expectTrue(!fuse::net::needs_correction(std::numeric_limits<float>::quiet_NaN(), 0.1f),
               "non-finite error returns false");
    expectTrue(fuse::net::needs_correction(0.2f, -1.f), "negative threshold counts as 0");
    expectTrue(!fuse::net::needs_correction(0.f, -1.f), "zero error stays within a clamped threshold");
}

void testReconcileErrOk() {
    expectTrue(fuse::net::reconcile_err_ok(0.f), "reconcile_err_ok(0) is true");
    expectTrue(fuse::net::reconcile_err_ok(1.5f), "reconcile_err_ok(1.5) is true");
    expectTrue(fuse::net::reconcile_err_ok(10.f), "reconcile_err_ok(10) is true");
    expectTrue(!fuse::net::reconcile_err_ok(-0.1f), "reconcile_err_ok(-0.1) is false");
    expectTrue(!fuse::net::reconcile_err_ok(10.1f), "reconcile_err_ok(10.1) is false");
    expectTrue(!fuse::net::reconcile_err_ok(std::numeric_limits<float>::quiet_NaN()),
               "reconcile_err_ok(NaN) is false");
}

void run_reconcile_tests() {
    const int before = g_failures;
    testReconcileErrOk();
    if (g_failures != before) {
        return;
    }

    testPredictionError();

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
}

} // namespace fuse::net::tests
