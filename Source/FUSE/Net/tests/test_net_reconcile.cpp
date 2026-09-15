#include <fuse/net/input_history.hpp>
#include <fuse/net/reconcile.hpp>

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
}

} // namespace fuse::net::tests
