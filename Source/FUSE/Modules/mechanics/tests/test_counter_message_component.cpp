#include <fuse/mechanics/counter_component.hpp>
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

} // namespace

int main() {
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
