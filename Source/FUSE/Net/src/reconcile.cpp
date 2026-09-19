#include <fuse/net/reconcile.hpp>

#include <fuse/net/rollback_buffer.hpp>

namespace fuse::net {

bool is_empty_rollback_buffer(const RollbackBuffer& buffer) {
    return buffer.empty();
}

InputReconcilePreflight preflight_input_reconcile(const InputHistoryBuffer& history, u32 frame) {
    InputReconcilePreflight preflight{};
    preflight.zero_capacity = history.capacity() == 0;
    if (preflight.zero_capacity) {
        return preflight;
    }

    preflight.history_empty = history.empty();
    if (preflight.history_empty) {
        return preflight;
    }

    if (frame < history.oldest_stored_frame()) {
        preflight.frame_before_oldest = true;
    }
    if (frame > history.newest_stored_frame()) {
        preflight.frame_beyond_newest = true;
    }
    return preflight;
}

bool should_skip_input_reconcile(const InputHistoryBuffer& history, u32 frame) {
    return preflight_input_reconcile(history, frame).should_skip();
}

RollbackReconcilePreflight preflight_rollback_reconcile(const RollbackBuffer& buffer, u32 frame) {
    RollbackReconcilePreflight preflight{};
    preflight.zero_capacity = buffer.capacity() == 0;
    if (preflight.zero_capacity) {
        return preflight;
    }

    preflight.buffer_empty = buffer.empty();
    if (preflight.buffer_empty) {
        return preflight;
    }

    if (frame < buffer.oldest_stored_frame()) {
        preflight.frame_before_oldest = true;
    }
    if (frame > buffer.newest_stored_frame()) {
        preflight.frame_beyond_newest = true;
    }

    preflight.has_snapshot = buffer.has_frame(frame);
    return preflight;
}

bool should_skip_rollback_reconcile(const RollbackBuffer& buffer, u32 frame) {
    return preflight_rollback_reconcile(buffer, frame).should_skip();
}

bool can_reconcile_input_frame(const InputHistoryBuffer& history, u32 frame) {
    return preflight_input_reconcile(history, frame).can_reconcile();
}

bool can_reconcile_rollback_frame(const RollbackBuffer& buffer, u32 frame) {
    return preflight_rollback_reconcile(buffer, frame).can_reconcile();
}

ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                          const PlayerInput& authoritative) {
    ReconcileResult result{};
    result.frame = frame;

    if (should_skip_input_reconcile(history, frame)) {
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

    if (should_skip_rollback_reconcile(buffer, frame)) {
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
