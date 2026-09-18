#include <fuse/net/reconcile.hpp>

#include <fuse/net/rollback_buffer.hpp>

namespace fuse::net {

namespace {

bool input_frame_in_window_(const InputHistoryBuffer& history, u32 frame) {
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

bool rollback_frame_in_window_(const RollbackBuffer& buffer, u32 frame) {
    if (buffer.empty()) {
        return false;
    }

    if (frame < buffer.oldest_stored_frame()) {
        return false;
    }

    if (frame > buffer.newest_stored_frame()) {
        return false;
    }

    return true;
}

} // namespace

ReconcileInputPreflight preflight_reconcile_input(const InputHistoryBuffer& history, u32 frame) {
    ReconcileInputPreflight result{};
    result.capacity_ok = history.capacity() > 0;
    result.buffer_empty = history.empty();
    result.frame_in_window = result.capacity_ok && input_frame_in_window_(history, frame);
    result.has_retained_frame = !result.buffer_empty && history.has_frame(frame);
    result.has_prediction = history.has_predicted(frame);
    return result;
}

ReconcileRollbackPreflight preflight_reconcile_rollback(const RollbackBuffer& buffer, u32 frame) {
    ReconcileRollbackPreflight result{};
    result.capacity_ok = buffer.capacity() > 0;
    result.buffer_empty = buffer.empty();
    result.frame_in_window = result.capacity_ok && rollback_frame_in_window_(buffer, frame);
    result.has_snapshot = buffer.has_frame(frame);
    result.has_local_prediction = buffer.has_local_input(frame);
    return result;
}

bool should_skip_reconcile_input(const InputHistoryBuffer& history, u32 frame) {
    return !preflight_reconcile_input(history, frame).can_reconcile();
}

bool should_skip_reconcile_rollback(const RollbackBuffer& buffer, u32 frame) {
    return !preflight_reconcile_rollback(buffer, frame).can_reconcile();
}

bool can_reconcile_input_frame(const InputHistoryBuffer& history, u32 frame) {
    const ReconcileInputPreflight preflight = preflight_reconcile_input(history, frame);
    if (!preflight.can_reconcile()) {
        return false;
    }

    if (!preflight.buffer_empty && !preflight.has_retained_frame) {
        return false;
    }

    return true;
}

bool can_reconcile_rollback_frame(const RollbackBuffer& buffer, u32 frame) {
    return preflight_reconcile_rollback(buffer, frame).can_reconcile();
}

ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                          const PlayerInput& authoritative) {
    ReconcileResult result{};
    result.frame = frame;

    if (!can_reconcile_input_frame(history, frame)) {
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

    if (!can_reconcile_rollback_frame(buffer, frame)) {
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
