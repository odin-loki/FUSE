#include <fuse/net/reconcile.hpp>

#include <fuse/net/rollback_buffer.hpp>

namespace fuse::net {

InputReconcilePreflight preflight_reconcile_input(const InputHistoryBuffer& history, u32 frame) {
    InputReconcilePreflight result;
    result.zero_capacity = history.capacity() == 0;
    if (result.zero_capacity) {
        return result;
    }

    result.ring_empty = history.empty();
    if (result.ring_empty) {
        result.frame_in_window = true;
        return result;
    }

    result.frame_evicted = frame < history.oldest_stored_frame();
    result.frame_future = frame > history.newest_stored_frame();
    result.frame_in_window = !result.frame_evicted && !result.frame_future;
    if (result.frame_in_window) {
        result.has_prediction = history.has_predicted(frame);
    }
    return result;
}

RollbackReconcilePreflight preflight_reconcile_rollback(const RollbackBuffer& buffer, u32 frame) {
    RollbackReconcilePreflight result;
    result.zero_capacity = buffer.capacity() == 0;
    if (result.zero_capacity) {
        return result;
    }

    result.ring_empty = buffer.empty();
    if (result.ring_empty) {
        return result;
    }

    result.frame_evicted = frame < buffer.oldest_stored_frame();
    result.frame_future = frame > buffer.newest_stored_frame();
    result.frame_in_window = !result.frame_evicted && !result.frame_future;
    if (!result.frame_in_window) {
        return result;
    }

    result.has_snapshot = buffer.has_frame(frame);
    if (result.has_snapshot) {
        result.has_local_input = buffer.has_local_input(frame);
    }
    return result;
}

bool should_skip_reconcile_input(const InputHistoryBuffer& history, u32 frame) {
    return !preflight_reconcile_input(history, frame).can_reconcile();
}

bool should_skip_reconcile_rollback(const RollbackBuffer& buffer, u32 frame) {
    return !preflight_reconcile_rollback(buffer, frame).can_reconcile();
}

bool can_reconcile_input(const InputHistoryBuffer& history, u32 frame) {
    return preflight_reconcile_input(history, frame).can_reconcile();
}

bool can_reconcile_rollback(const RollbackBuffer& buffer, u32 frame) {
    return preflight_reconcile_rollback(buffer, frame).can_reconcile();
}

bool can_reconcile_input_frame(const InputHistoryBuffer& history, u32 frame) {
    return can_reconcile_input(history, frame);
}

bool can_reconcile_rollback_frame(const RollbackBuffer& buffer, u32 frame) {
    return can_reconcile_rollback(buffer, frame);
}

ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                          const PlayerInput& authoritative) {
    ReconcileResult result{};
    result.frame = frame;

    if (should_skip_reconcile_input(history, frame)) {
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

    if (should_skip_reconcile_rollback(buffer, frame)) {
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
