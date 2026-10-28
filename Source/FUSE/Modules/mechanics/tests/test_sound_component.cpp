#include <fuse/core/init.hpp>
#include <fuse/mechanics/sound_component.hpp>
#include <fuse/mechanics/sound_db.hpp>
#include <fuse/mechanics/sound_gain.hpp>

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testSoundDbOk() {
    expectTrue(fuse::mechanics::sound_db_ok(-80.f), "sound_db_ok(-80) is true");
    expectTrue(fuse::mechanics::sound_db_ok(0.f), "sound_db_ok(0) is true");
    expectTrue(fuse::mechanics::sound_db_ok(12.f), "sound_db_ok(12) is true");
    expectTrue(!fuse::mechanics::sound_db_ok(-80.1f), "sound_db_ok(-80.1) is false");
    expectTrue(!fuse::mechanics::sound_db_ok(12.1f), "sound_db_ok(12.1) is false");
    const float nonFinite = std::numeric_limits<float>::quiet_NaN();
    expectTrue(!fuse::mechanics::sound_db_ok(nonFinite), "sound_db_ok(non-finite) is false");
}

void testClampSoundGain() {
    expectTrue(fuse::mechanics::clamp_sound_gain(0.5f) == 0.5f, "clamp_sound_gain(0.5) == 0.5");
    expectTrue(fuse::mechanics::clamp_sound_gain(-1.f) == 0.f, "clamp_sound_gain(-1) == 0");
    expectTrue(fuse::mechanics::clamp_sound_gain(2.f) == 1.f, "clamp_sound_gain(2) == 1");
    expectTrue(fuse::mechanics::clamp_sound_gain(std::numeric_limits<float>::quiet_NaN()) == 0.f,
               "non-finite gain returns 0");
    expectTrue(fuse::mechanics::clamp_sound_gain(std::numeric_limits<float>::infinity()) == 0.f,
               "infinite gain returns 0");
}

} // namespace

int main() {
    testSoundDbOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mechanics_sound_component: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testClampSoundGain();
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
