#include <fuse/adventure/examine_interactable.hpp>
#include <fuse/adventure/interaction.hpp>
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

void testExamineInteractable() {
    fuse::adventure::ExamineInteractable plaque("Ancient runes cover the stone.");
    fuse::adventure::InteractionSystem system;

    fuse::adventure::InteractContext ctx;
    ctx.actorName = "player";

    const fuse::adventure::InteractResult result = system.examine(ctx, plaque);
    expectTrue(result == fuse::adventure::InteractResult::Examined, "examine succeeds");
    expectTrue(plaque.examineCount() == 1u, "examine count increments");
    expectTrue(plaque.lastExaminedBy() == "player", "examiner recorded");
    expectTrue(!plaque.description().empty(), "lore text preserved");
}

} // namespace

int main() {
    fuse::core::initialize();
    testExamineInteractable();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure_examine_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure_examine_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
