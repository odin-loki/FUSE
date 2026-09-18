#include <fuse/net/reconcile.hpp>

#include <fuse/net/rollback_buffer.hpp>

namespace fuse::net {

bool input_frame_matches(u32 frame, const PlayerInput& input) {
    return input.frame == frame;
}

bool can_reconcile_input_frame(const InputHistoryBuffer& history, u32 frame) {
    if (history.capacity() == 0) {
        return false;
    }

    if (history.empty()) {
        return true;
    }

    if (frame < history.oldest_stored_frame()) {
        return false;
    }

    if (frame > history.newest_stored_frame()) {
        return false;
    }

    return true;
}

bool can_reconcile_rollback_frame(const RollbackBuffer& buffer, u32 frame) {
    if (buffer.capacity() == 0 || buffer.empty()) {
        return false;
    }

    if (frame < buffer.oldest_stored_frame()) {
        return false;
    }

    if (frame > buffer.newest_stored_frame()) {
        return false;
    }

    return buffer.has_frame(frame);
}

InputReconcilePreflight preflight_reconcile_input(const InputHistoryBuffer& history, u32 frame,
                                                    const PlayerInput& input) {
    InputReconcilePreflight result{};
    result.history_empty = history.empty();
    result.buffer_capacity_ok = history.capacity() > 0;
    result.frame_in_window = can_reconcile_input_frame(history, frame);
    result.input_frame_ok = input_frame_matches(frame, input);
    result.has_prediction = history.has_predicted(frame);
    return result;
}

RollbackReconcilePreflight preflight_reconcile_rollback(const RollbackBuffer& buffer, u32 frame,
                                                          const PlayerInput& remote) {
    RollbackReconcilePreflight result{};
    result.buffer_empty = buffer.empty();
    result.buffer_capacity_ok = buffer.capacity() > 0;
    result.frame_in_window = can_reconcile_rollback_frame(buffer, frame);
    result.input_frame_ok = input_frame_matches(frame, remote);
    result.has_snapshot = buffer.has_frame(frame);
    result.has_local_input = buffer.has_local_input(frame);
    return result;
}

bool should_skip_input_reconcile(const InputHistoryBuffer& history, u32 frame, const PlayerInput& input) {
    const InputReconcilePreflight preflight = preflight_reconcile_input(history, frame, input);
    return !preflight.can_reconcile();
}

bool should_skip_rollback_reconcile(const RollbackBuffer& buffer, u32 frame, const PlayerInput& remote) {
    const RollbackReconcilePreflight preflight = preflight_reconcile_rollback(buffer, frame, remote);
    return !preflight.can_reconcile();
}

ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                          const PlayerInput& authoritative) {
    ReconcileResult result{};
    result.frame = frame;

    if (should_skip_input_reconcile(history, frame, authoritative)) {
        return result;
    }

    history.store_confirmed(frame, authoritative);

    if (!history.has_predicted(frame)) {
        result.action = ReconcileAction::NoOp;
        return result;
    }

    if (history.prediction_matches(frame)) {
        result.action = ReconcileAction::Confirmed;
        return result;
    }

    result.action = ReconcileAction::Mismatch;
    return result;
}

ReconcileResult reconcile_rollback_buffer(RollbackBuffer& buffer, u32 frame, const PlayerInput& remote) {
    ReconcileResult result{};
    result.frame = frame;

    if (should_skip_rollback_reconcile(buffer, frame, remote)) {
        return result;
    }

    buffer.store_remote_input(frame, remote, true);

    if (!buffer.has_local_input(frame)) {
        result.action = ReconcileAction::NoOp;
        return result;
    }

    if (buffer.inputs_match(frame)) {
        result.action = ReconcileAction::Confirmed;
        return result;
    }

    result.action = ReconcileAction::Mismatch;
    return result;
}

} // namespace fuse::net
