#include <fuse/core/init.hpp>

#include <cstdio>
#include <cstdlib>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_transport_tests();
void run_serializer_tests();
void run_rollback_tests();
void run_state_sync_tests();

} // namespace fuse::net::tests

int main() {
    fuse::core::initialize();

    fuse::net::tests::run_transport_tests();
    fuse::net::tests::run_serializer_tests();
    fuse::net::tests::run_rollback_tests();
    fuse::net::tests::run_state_sync_tests();

    fuse::core::shutdown();

    if (fuse::net::tests::g_failures == 0) {
        std::printf("fuse_net_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_net_tests: %d failure(s)\n", fuse::net::tests::g_failures);
    return EXIT_FAILURE;
}
