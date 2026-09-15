#include <fuse/net/input_history.hpp>
#include <fuse/net/reconcile.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_input_history_tests() {
    fuse::net::InputHistoryBuffer empty;
    empty.init(8);
    expectTrue(!empty.pop_oldest().has_value(), "pop_oldest on empty history returns nullopt");
    expectTrue(empty.stored_frame_count() == 0u, "empty history reports zero stored frames");
    expectTrue(empty.empty(), "fresh history reports empty");
    expectTrue(!empty.has_frame(0u), "empty history has no frames");

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

    fuse::net::InputHistoryBuffer push_pop_history;
    push_pop_history.init(4);
    for (fuse::u32 frame = 0; frame < 6; ++frame) {
        fuse::net::PlayerInput input{};
        input.frame = frame;
        input.buttons = frame + 1;
        push_pop_history.push_frame(frame, input);
    }

    expectTrue(push_pop_history.stored_frame_count() == 4u, "push_frame retains ring capacity");
    expectTrue(push_pop_history.oldest_stored_frame() == 2u, "push_frame evicts oldest on wrap");

    const std::optional<fuse::net::InputHistoryFrame> popped = push_pop_history.pop_oldest();
    expectTrue(popped.has_value(), "pop_oldest returns a frame");
    expectTrue(popped->frame == 2u, "pop_oldest returns oldest frame first");
    expectTrue(popped->has_predicted, "pop_oldest preserves predicted payload");
    expectTrue(popped->predicted.buttons == 3u, "pop_oldest payload matches stored frame");
    expectTrue(push_pop_history.stored_frame_count() == 3u, "pop_oldest shrinks retained count");
    expectTrue(!push_pop_history.has_frame(2u), "popped frame no longer queryable");

    fuse::net::InputHistoryBuffer cleared;
    cleared.init(4);
    cleared.push_frame(1, fuse::net::PlayerInput{});
    cleared.clear();
    expectTrue(cleared.stored_frame_count() == 0u, "clear resets stored frame count");
    expectTrue(!cleared.pop_oldest().has_value(), "pop_oldest after clear returns nullopt");

    fuse::net::InputHistoryBuffer wrap_reconcile;
    wrap_reconcile.init(4);
    for (fuse::u32 frame = 0; frame < 6; ++frame) {
        fuse::net::PlayerInput predicted{};
        predicted.frame = frame;
        predicted.axis_lx = static_cast<std::int16_t>(frame * 10);
        wrap_reconcile.push_frame(frame, predicted);
    }

    expectTrue(!wrap_reconcile.has_frame(1u), "evicted frame before oldest is not queryable");
    expectTrue(wrap_reconcile.has_frame(5u), "newest frame retained after wrap");

    fuse::net::PlayerInput authority{};
    authority.frame = 5;
    authority.axis_lx = 50;
    const fuse::net::ReconcileResult wrap_result = wrap_reconcile.reconcile_authoritative(5, authority);
    expectTrue(wrap_result.action == fuse::net::ReconcileAction::Confirmed,
               "reconcile succeeds for retained frame after ring wrap");
    expectTrue(wrap_reconcile.prediction_matches(5u), "prediction confirmed after wrap reconcile");

    fuse::net::PlayerInput evicted_authority{};
    evicted_authority.frame = 1;
    const fuse::net::ReconcileResult evicted_result =
        wrap_reconcile.reconcile_authoritative(1, evicted_authority);
    expectTrue(evicted_result.action == fuse::net::ReconcileAction::NoOp,
               "reconcile on evicted frame returns NoOp");
    expectTrue(!wrap_reconcile.has_confirmed(1u), "evicted frame is not restored by reconcile");

    fuse::net::InputHistoryBuffer zero_capacity;
    zero_capacity.init(4);
    zero_capacity.clear();
    const fuse::net::ReconcileResult zero_result =
        zero_capacity.reconcile_authoritative(0, fuse::net::PlayerInput{});
    expectTrue(zero_result.action == fuse::net::ReconcileAction::NoOp,
               "reconcile on cleared history returns NoOp");
    expectTrue(zero_capacity.stored_frame_count() == 0u, "cleared history reconcile has no side effects");
    expectTrue(!zero_capacity.has_confirmed(0u), "cleared history does not record reconcile input");

    fuse::net::InputHistoryBuffer evict_before_write;
    evict_before_write.init(4);
    for (fuse::u32 frame = 0; frame < 4; ++frame) {
        fuse::net::PlayerInput input{};
        input.frame = frame;
        input.buttons = frame + 10;
        evict_before_write.push_frame(frame, input);
    }
    expectTrue(evict_before_write.has_frame(0u), "frame 0 retained before wrap write");
    expectTrue(evict_before_write.predicted(0u).buttons == 10u, "frame 0 payload retained before wrap");

    fuse::net::PlayerInput wrap_input{};
    wrap_input.frame = 4;
    wrap_input.buttons = 99;
    evict_before_write.push_frame(4, wrap_input);

    expectTrue(!evict_before_write.has_frame(0u), "evict-before-write clears wrapped slot");
    expectTrue(evict_before_write.has_frame(4u), "wrap write retains newest frame");
    expectTrue(evict_before_write.predicted(4u).buttons == 99u, "wrap write stores new payload");
    expectTrue(evict_before_write.oldest_stored_frame() == 1u, "evict-before-write advances oldest frame");
    expectTrue(!evict_before_write.empty(), "non-empty history after wrap write");

    fuse::net::InputHistoryBuffer bounded;
    bounded.init(4);
    for (fuse::u32 frame = 2; frame < 6; ++frame) {
        fuse::net::PlayerInput input{};
        input.frame = frame;
        bounded.push_frame(frame, input);
    }
    expectTrue(fuse::net::can_reconcile_input_frame(bounded, 5u),
               "can_reconcile accepts newest retained frame");
    expectTrue(!fuse::net::can_reconcile_input_frame(bounded, 1u),
               "can_reconcile rejects evicted frame");
    expectTrue(!fuse::net::can_reconcile_input_frame(bounded, 6u),
               "can_reconcile rejects future frame beyond newest");
}

} // namespace fuse::net::tests
