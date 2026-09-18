#include <fuse/net/reconcile.hpp>
#include <fuse/net/rollback_buffer.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_rollback_buffer_tests() {
    fuse::net::RollbackBuffer empty;
    empty.init(8);
    expectTrue(empty.stored_frame_count() == 0u, "empty buffer reports zero stored frames");
    expectTrue(empty.empty(), "fresh rollback buffer reports empty");
    expectTrue(!empty.evict_oldest_snapshot().has_value(), "evict_oldest on empty buffer returns nullopt");
    expectTrue(!empty.has_local_input(0u), "empty buffer has no local input");

    fuse::net::RollbackBuffer buffer;
    buffer.init(8);

    fuse::net::GameSnapshot frame0;
    frame0.frame = 0;
    frame0.checksum = 10;
    frame0.ecs_state = {1, 2, 3};
    buffer.store_snapshot(0, frame0);

    fuse::net::GameSnapshot frame1 = frame0;
    frame1.frame = 1;
    frame1.checksum = 11;
    frame1.ecs_state = {4, 5, 6};
    buffer.store_snapshot(1, frame1);

    expectTrue(buffer.has_frame(0), "frame 0 stored");
    expectTrue(buffer.has_frame(1), "frame 1 stored");
    expectTrue(!buffer.has_frame(2), "frame 2 not stored");
    expectTrue(buffer.oldest_stored_frame() == 0, "oldest frame tracked");
    expectTrue(buffer.newest_stored_frame() == 1, "newest frame tracked");
    expectTrue(buffer.stored_frame_count() == 2u, "stored frame count tracks span");

    const fuse::net::GameSnapshot* loaded = buffer.snapshot(1);
    expectTrue(loaded != nullptr, "snapshot lookup succeeds");
    expectTrue(loaded->checksum == 11, "snapshot checksum preserved");

    fuse::net::PlayerInput local{};
    local.frame = 1;
    local.axis_lx = 42;
    buffer.store_local_input(1, local);
    expectTrue(buffer.has_local_input(1), "local input presence tracked");
    expectTrue(buffer.local_input(1).axis_lx == 42, "local input stored in slot");
    expectTrue(!buffer.has_local_input(0), "local input absent on other frames");

    fuse::net::PlayerInput remote{};
    remote.frame = 1;
    remote.axis_lx = 99;
    buffer.store_remote_input(1, remote, true);
    expectTrue(buffer.remote_confirmed(1), "remote input marked confirmed");
    expectTrue(buffer.remote_input(1).axis_lx == 99, "remote input stored in slot");
    expectTrue(!buffer.inputs_match(1), "inputs_match false when local and remote differ");

    remote.axis_lx = 42;
    buffer.store_remote_input(1, remote, true);
    expectTrue(buffer.inputs_match(1), "inputs_match true when payloads align");

    for (fuse::u32 frame = 0; frame < 10; ++frame) {
        fuse::net::GameSnapshot snap = frame0;
        snap.frame = frame;
        snap.checksum = frame;
        buffer.store_snapshot(frame, snap);
    }

    expectTrue(buffer.capacity() == 8, "capacity clamped to requested size");
    expectTrue(buffer.oldest_stored_frame() == 2, "oldest snapshot evicted after ring wrap");
    expectTrue(buffer.newest_stored_frame() == 9, "newest snapshot tracked across wrap");
    expectTrue(buffer.stored_frame_count() == 8u, "stored frame count capped at capacity");
    expectTrue(!buffer.has_frame(1), "evicted snapshot no longer queryable");

    const std::optional<fuse::net::GameSnapshot> evicted = buffer.evict_oldest_snapshot();
    expectTrue(evicted.has_value(), "evict_oldest returns oldest snapshot");
    expectTrue(evicted->frame == 2u, "evict_oldest returns oldest frame first");
    expectTrue(buffer.stored_frame_count() == 7u, "evict_oldest shrinks retained count");
    expectTrue(!buffer.has_frame(2u), "evicted frame no longer queryable");
    expectTrue(!buffer.empty(), "rollback buffer non-empty after snapshot store");

    // --- remaining capacity and preflight helpers (B7.4 deepen follow-up) ---
    fuse::net::RollbackBuffer capacity_buffer;
    capacity_buffer.init(4);
    expectTrue(capacity_buffer.remaining_capacity() == 4u, "fresh rollback buffer has full remaining capacity");
    for (fuse::u32 frame = 0; frame < 3; ++frame) {
        fuse::net::GameSnapshot snap{};
        snap.frame = frame;
        capacity_buffer.store_snapshot(frame, snap);
    }
    expectTrue(capacity_buffer.remaining_capacity() == 1u,
               "rollback remaining capacity shrinks as snapshots are stored");

    fuse::net::RollbackBuffer cleared_capacity;
    cleared_capacity.init(4);
    cleared_capacity.clear();
    expectTrue(cleared_capacity.remaining_capacity() == 0u,
               "cleared rollback buffer has zero remaining capacity");

    fuse::net::RollbackBuffer preflight_wrap;
    preflight_wrap.init(4);
    for (fuse::u32 frame = 0; frame < 5; ++frame) {
        fuse::net::GameSnapshot snap{};
        snap.frame = frame;
        preflight_wrap.store_snapshot(frame, snap);
    }
    const fuse::net::ReconcileRollbackPreflight wrap_preflight = preflight_wrap.preflight_remote_reconcile(4u);
    expectTrue(wrap_preflight.can_reconcile(), "preflight_remote_reconcile accepts newest wrapped frame");
    expectTrue(wrap_preflight.has_snapshot, "preflight_remote_reconcile sees wrapped snapshot");
    expectTrue(!preflight_wrap.should_skip_reconcile(4u), "should_skip false for wrapped newest frame");
    expectTrue(preflight_wrap.should_skip_reconcile(0u), "should_skip true for evicted wrapped frame");
}

} // namespace fuse::net::tests
