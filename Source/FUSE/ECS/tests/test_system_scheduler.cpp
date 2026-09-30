#include <fuse/ecs/system_scheduler.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

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

/// GAP-GAME-LOOP-ECS: ties break by registration order (no hash-order dependence), the compiled
/// order is cached, Inline mode runs on the calling thread, cycles throw.
void testDeterministicInlineOrder() {
    fuse::ecs::SystemScheduler systems;
    systems.set_execution_mode(fuse::ecs::SystemScheduler::ExecutionMode::Inline);
    std::vector<int> order;
    order.reserve(16);
    // Registered out of dependency order: d depends on b; a, b, c independent.
    systems.register_system({"d", [&]() { order.push_back(3); }, {"b"}});
    systems.register_system({"a", [&]() { order.push_back(0); }, {}});
    systems.register_system({"b", [&]() { order.push_back(1); }, {}});
    systems.register_system({"c", [&]() { order.push_back(2); }, {"missing"}});
    systems.register_system({"a", [&]() { order.push_back(99); }, {}}); // duplicate name ignored
    expectTrue(systems.system_count() == 4u, "duplicate system name rejected");
    for (int run = 0; run < 3; ++run) {
        order.clear();
        systems.run_all();
        const std::vector<int> expected = {0, 1, 3, 2};
        expectTrue(order == expected, "ties run in registration order after their dependencies");
    }
    expectTrue(systems.run_count() == 3u, "run_count counts runs");
    const std::vector<fuse::u32>& compiled = systems.execution_order();
    expectTrue(compiled.size() == 4u && systems.system_name(compiled[2]) == "d", "execution_order exposes the order");

    fuse::ecs::SystemScheduler cyclic;
    cyclic.register_system({"x", []() {}, {"y"}});
    cyclic.register_system({"y", []() {}, {"x"}});
    bool threw = false;
    try {
        cyclic.run_all();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expectTrue(threw, "cyclic dependencies throw");
}

} // namespace

int main() {
    testDependencyOrder();
    testDeterministicInlineOrder();

    if (g_failures == 0) {
        std::printf("fuse_ecs system scheduler tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs system scheduler tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
