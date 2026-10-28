#include <fuse/core/init.hpp>
#include <fuse/net/jitter_delay.hpp>
#include <fuse/net/suite_name.hpp>

#include <cstdio>
#include <cstdlib>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void testJitterDelay() {
    expectTrue(fuse::net::jitter_delay(10, 5) == 15, "jitter_delay(10, 5) is 15");
    expectTrue(fuse::net::jitter_delay(1000, 0) == 1000, "jitter_delay(1000, 0) is 1000");
    expectTrue(fuse::net::jitter_delay(1001, 0) == -1, "jitter_delay(1001, 0) is -1");
    expectTrue(fuse::net::jitter_delay(10, 201) == -1, "jitter_delay(10, 201) is -1");
}

void testSuiteNameOk() {
    expectTrue(fuse::net::suite_name_ok("Sync"), "suite_name_ok(Sync) is true");
    expectTrue(!fuse::net::suite_name_ok(""), "suite_name_ok(empty) is false");
    expectTrue(!fuse::net::suite_name_ok("1Sync"), "suite_name_ok(1Sync) is false");
    expectTrue(!fuse::net::suite_name_ok("Net Sync"), "suite_name_ok(Net Sync) is false");
}

void run_transport_tests();
void run_serializer_tests();
void run_rollback_tests();
void run_rollback_window_tests();
void run_rollback_buffer_tests();
void run_checksum_tests();
void run_input_history_tests();
void run_reconcile_tests();
void run_snapshot_delta_tests();
void run_state_sync_tests();
void run_interest_management_tests();

} // namespace fuse::net::tests

int main() {
    fuse::net::tests::testJitterDelay();
    if (fuse::net::tests::g_failures != 0) {
        std::fprintf(stderr, "fuse_net_tests: %d failure(s)\n", fuse::net::tests::g_failures);
        return EXIT_FAILURE;
    }

    fuse::net::tests::testSuiteNameOk();
    if (fuse::net::tests::g_failures != 0) {
        std::fprintf(stderr, "fuse_net_tests: %d failure(s)\n", fuse::net::tests::g_failures);
        return EXIT_FAILURE;
    }

    fuse::core::initialize();

    fuse::net::tests::run_transport_tests();
    fuse::net::tests::run_serializer_tests();
    fuse::net::tests::run_rollback_tests();
    fuse::net::tests::run_rollback_window_tests();
    fuse::net::tests::run_rollback_buffer_tests();
    fuse::net::tests::run_checksum_tests();
    fuse::net::tests::run_input_history_tests();
    fuse::net::tests::run_reconcile_tests();
    fuse::net::tests::run_snapshot_delta_tests();
    fuse::net::tests::run_state_sync_tests();
    fuse::net::tests::run_interest_management_tests();

    fuse::core::shutdown();

    if (fuse::net::tests::g_failures == 0) {
        std::printf("fuse_net_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_net_tests: %d failure(s)\n", fuse::net::tests::g_failures);
    return EXIT_FAILURE;
}
