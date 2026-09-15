#include <fuse/net/rollback_buffer.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_rollback_buffer_tests() {
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

    const fuse::net::GameSnapshot* loaded = buffer.snapshot(1);
    expectTrue(loaded != nullptr, "snapshot lookup succeeds");
    expectTrue(loaded->checksum == 11, "snapshot checksum preserved");

    fuse::net::PlayerInput local{};
    local.frame = 1;
    local.axis_lx = 42;
    buffer.store_local_input(1, local);
    expectTrue(buffer.local_input(1).axis_lx == 42, "local input stored in slot");

    fuse::net::PlayerInput remote{};
    remote.frame = 1;
    remote.axis_lx = 99;
    buffer.store_remote_input(1, remote, true);
    expectTrue(buffer.remote_confirmed(1), "remote input marked confirmed");
    expectTrue(buffer.remote_input(1).axis_lx == 99, "remote input stored in slot");
}

} // namespace fuse::net::tests
