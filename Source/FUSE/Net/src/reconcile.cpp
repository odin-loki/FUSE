#include <fuse/net/reconcile.hpp>

namespace fuse::net {

ReconcileResult reconcile_predicted_input(InputHistoryBuffer& history, u32 frame,
                                          const PlayerInput& authoritative) {
    ReconcileResult result{};
    result.frame = frame;

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

} // namespace fuse::net
