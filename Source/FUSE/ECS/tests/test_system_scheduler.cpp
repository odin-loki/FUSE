#include <fuse/ecs/sched_phase.hpp>
#include <fuse/ecs/system_name.hpp>
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

void testSchedPhaseOk() {
    expectTrue(fuse::ecs::sched_phase_ok(0), "phase 0 is a valid schedule phase");
    expectTrue(fuse::ecs::sched_phase_ok(7), "phase 7 is a valid schedule phase");
    expectTrue(!fuse::ecs::sched_phase_ok(-1), "phase -1 is an invalid schedule phase");
    expectTrue(!fuse::ecs::sched_phase_ok(8), "phase 8 is an invalid schedule phase");
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

void testSystemNameOk() {
    expectTrue(fuse::ecs::system_name_ok("TransformSystem"), "TransformSystem is a valid system name");
    expectTrue(!fuse::ecs::system_name_ok(""), "empty system name is invalid");
    expectTrue(!fuse::ecs::system_name_ok("2D"), "leading digit is an invalid system name");
    expectTrue(!fuse::ecs::system_name_ok("_Hidden"), "leading underscore is an invalid system name");
    expectTrue(!fuse::ecs::system_name_ok("Camera System"), "space is an invalid system name character");
    expectTrue(fuse::ecs::system_name_ok("A"), "single letter is a valid system name");
    expectTrue(fuse::ecs::system_name_ok("transformSystem"), "lowercase first letter is a valid system name");
    expectTrue(fuse::ecs::system_name_ok("Camera_System2"), "underscore and digit are valid after the first letter");
    expectTrue(fuse::ecs::system_name_ok(std::string(40, 'A')), "40-character system name is valid");
    expectTrue(!fuse::ecs::system_name_ok(std::string(41, 'A')), "41-character system name is invalid");
}

} // namespace

int main() {
    const int before = g_failures;
    testSchedPhaseOk();
    if (g_failures != before) {
        std::fprintf(stderr, "fuse_ecs system scheduler tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testSystemNameOk();
    testDependencyOrder();
    testDeterministicInlineOrder();

    if (g_failures == 0) {
        std::printf("fuse_ecs system scheduler tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs system scheduler tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

#include <fuse/ecs/schedule_order.hpp>

namespace {

void testScheduleOrder() {
    expectTrue(fuse::ecs::system_before(10, 20), "Order 10 is before 20");
    expectTrue(fuse::ecs::system_phase(10) == 1, "Order 10 is phase 1");
    expectTrue(fuse::ecs::system_phase(-5) == 0, "Order -5 is phase 0");
    expectTrue(fuse::ecs::system_phase(100) == 2, "Order 100 is phase 2");
}

// Startup registration so main's failure count includes this check.
struct ScheduleOrderTestRegistration {
    ScheduleOrderTestRegistration() { testScheduleOrder(); }
};

ScheduleOrderTestRegistration g_scheduleOrderTestRegistration;

} // namespace
