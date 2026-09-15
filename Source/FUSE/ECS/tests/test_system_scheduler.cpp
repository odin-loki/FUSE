#include <fuse/ecs/system_scheduler.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testDependencyOrder() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    fuse::ecs::SystemScheduler systems;
    std::vector<std::string> order;
    std::atomic<bool> transform_ran{false};

    systems.register_system({
        "TransformSystem",
        [&]() {
            order.push_back("TransformSystem");
            transform_ran.store(true, std::memory_order_release);
        },
        {},
    });
    systems.register_system({
        "CameraSystem",
        [&]() {
            expectTrue(transform_ran.load(std::memory_order_acquire), "camera waits for transform");
            order.push_back("CameraSystem");
        },
        {"TransformSystem"},
    });

    systems.run_all();
    expectTrue(order.size() == 2u, "both systems ran");
    expectTrue(order[0] == "TransformSystem", "transform runs before camera");

    scheduler.shutdown();
}

} // namespace

int main() {
    testDependencyOrder();

    if (g_failures == 0) {
        std::printf("fuse_ecs system scheduler tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs system scheduler tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
