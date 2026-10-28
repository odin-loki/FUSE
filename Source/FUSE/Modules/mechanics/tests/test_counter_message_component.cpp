#include <fuse/mechanics/counter_cap.hpp>
#include <fuse/mechanics/counter_component.hpp>
#include <fuse/mechanics/counter_target.hpp>
#include <fuse/mechanics/message_component.hpp>
#include <fuse/core/init.hpp>

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

void testCounterReached() {
    expectTrue(fuse::mechanics::counter_reached(3, 3), "counter at target is reached");
    expectTrue(!fuse::mechanics::counter_reached(2, 3), "counter below target is not reached");
    expectTrue(fuse::mechanics::counter_reached(4, 3), "counter above target is reached");
}

void testCounterCapOk() {
    expectTrue(fuse::mechanics::counter_cap_ok(1), "counter_cap_ok(1) is true");
    expectTrue(fuse::mechanics::counter_cap_ok(1000000), "counter_cap_ok(1000000) is true");
    expectTrue(!fuse::mechanics::counter_cap_ok(0), "counter_cap_ok(0) is false");
    expectTrue(!fuse::mechanics::counter_cap_ok(1000001), "counter_cap_ok(1000001) is false");
}

} // namespace

int main() {
    testCounterCapOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mechanics_counter_message: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testCounterReached();
    fuse::core::initialize();

    fuse::mechanics::CounterComponent counter("test_counter", 0, 2);
    counter.increment();
    expectTrue(counter.value() == 1, "counter incremented");
    expectTrue(!counter.reachedTarget(), "counter below target");
    counter.increment();
    expectTrue(counter.reachedTarget(), "counter reached target");

    fuse::mechanics::MessageComponent message("test_message", "hello");
    message.send();
    expectTrue(message.sendCount() == 1u, "message sent");
    expectTrue(message.sentMessages().size() == 1u, "message stored");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_counter_message: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_counter_message: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
