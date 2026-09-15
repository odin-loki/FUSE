#include <fuse/net/input_history.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_input_history_tests() {
    fuse::net::InputHistoryBuffer history;
    history.init(16);

    fuse::net::PlayerInput predicted{};
    predicted.frame = 3;
    predicted.player_id = 1;
    predicted.axis_lx = 1200;
    history.store_predicted(3, predicted);

    expectTrue(history.has_predicted(3), "predicted input stored");
    expectTrue(!history.has_confirmed(3), "confirmed not stored yet");
    expectTrue(history.predicted(3).axis_lx == 1200, "predicted payload preserved");

    fuse::net::PlayerInput confirmed = predicted;
    history.store_confirmed(3, confirmed);
    expectTrue(history.has_confirmed(3), "confirmed input stored");
    expectTrue(history.prediction_matches(3), "matching prediction confirmed");

    confirmed.axis_lx = 9000;
    history.store_confirmed(4, predicted);
    history.store_predicted(4, predicted);
    history.store_confirmed(4, confirmed);
    expectTrue(!history.prediction_matches(4), "mismatched prediction detected");

    for (fuse::u32 frame = 0; frame < 20; ++frame) {
        fuse::net::PlayerInput input{};
        input.frame = frame;
        input.buttons = frame;
        history.store_predicted(frame, input);
    }

    expectTrue(history.capacity() == 16, "capacity clamped to requested size");
    expectTrue(history.oldest_stored_frame() == 4, "oldest frame evicted after ring wrap");
    expectTrue(history.newest_stored_frame() == 19, "newest frame tracked across wrap");
    expectTrue(history.has_predicted(19), "newest predicted frame retained");
    expectTrue(!history.has_predicted(3), "evicted frame no longer queryable");

    fuse::net::PlayerInput a{};
    a.buttons = 1;
    fuse::net::PlayerInput b = a;
    b.buttons = 2;
    expectTrue(fuse::net::inputs_equal(a, a), "inputs_equal reflexive");
    expectTrue(!fuse::net::inputs_equal(a, b), "inputs_equal detects button diff");
}

} // namespace fuse::net::tests
