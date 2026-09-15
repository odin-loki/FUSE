#include <fuse/net/reconcile.hpp>

#include <fuse/net/rollback_buffer.hpp>

namespace fuse::net {

ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                          const PlayerInput& authoritative) {
    ReconcileResult result{};
    result.frame = frame;

    if (history.capacity() == 0) {
        return result;
    }

    if (history.stored_frame_count() > 0) {
        if (frame < history.oldest_stored_frame()) {
            return result;
        }
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

    if (buffer.capacity() == 0) {
        return result;
    }

    if (buffer.stored_frame_count() > 0 && frame < buffer.oldest_stored_frame()) {
        return result;
    }

    if (!buffer.has_frame(frame)) {
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
