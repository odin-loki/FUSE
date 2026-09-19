#include <fuse/core/init.hpp>
#include <fuse/mechanics/sound_component.hpp>

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

    fuse::mechanics::SoundComponent sound("lever_sound", "audio/lever_click");
    sound.play();
    expectTrue(sound.playCount() == 1u, "sound component play counted");
    expectTrue(sound.playing(), "sound component playing flag");
    sound.stop();
    expectTrue(!sound.playing(), "sound component stopped");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_sound_component: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_sound_component: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
