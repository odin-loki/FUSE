#include <fuse/net/reconcile.hpp>

#include <fuse/net/rollback_buffer.hpp>

namespace fuse::net {

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
