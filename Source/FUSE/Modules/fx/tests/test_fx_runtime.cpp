#include <fuse/core/init.hpp>
#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/fx/spell_descriptor.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

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

void testRegisterDescriptors() {
    fuse::fx::FxComposer composer;
    const fuse::fx::EffectDescriptor spark = fuse::fx::EffectDescriptor::makeSparkBurst();
    const fuse::fx::SpellDescriptor fireball = fuse::fx::SpellDescriptor::makeFireball();

    expectTrue(composer.registerEffect(spark), "spark effect registers");
    expectTrue(composer.registerSpell(fireball), "fireball spell registers");
    expectTrue(composer.effectCount() == 1u, "one effect descriptor");
    expectTrue(composer.spellCount() == 1u, "one spell descriptor");
    expectTrue(composer.findEffect("spark_burst") != nullptr, "spark effect lookup");
    expectTrue(composer.findSpell("fireball") != nullptr, "fireball spell lookup");
}

void testTickEffectAttachment() {
    fuse::fx::FxComposer composer;
    composer.registerEffect(fuse::fx::EffectDescriptor::makeSparkBurst());

    fuse::fx::FxSocket socket;
    socket.kind = fuse::fx::FxSocketKind::Sprite2D;
    socket.effectId = "spark_burst";
    composer.attach(socket);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    ctx.frameIndex = 1;

    for (fuse::u32 frame = 0; frame < 5; ++frame) {
        composer.tick(ctx);
        ++ctx.frameIndex;
    }

    expectTrue(composer.attachmentCount() == 1u, "one socket attached");
    expectTrue(composer.tickCount() == 5u, "composer tick count advanced");
}

void testCastPipelineAndResiduals() {
    fuse::fx::FxComposer composer;
    composer.registerSpell(fuse::fx::SpellDescriptor::makeFireball());

    fuse::u32 residualSpawns = 0;
    composer.residuals().setSpawnHook([&](const fuse::fx::ResidualEntry& entry) {
        ++residualSpawns;
        expectTrue(entry.kind == fuse::fx::ResidualKind::Zodiac, "residual spawn hook invoked");
    });

    fuse::fx::CastBinding binding;
    binding.caster = fuse::Handle<fuse::Object>(1, 1);
    binding.target = fuse::Handle<fuse::Object>(2, 1);

    expectTrue(composer.beginCast("fireball", binding), "fireball cast begins");
    expectTrue(composer.castPipeline().activeCount() == 1u, "one active cast");

    fuse::frame::FrameCtx ctx;
    ctx.dt = 0.2f;

    while (composer.castPipeline().activeCount() > 0) {
        composer.tick(ctx);
    }

    expectTrue(composer.castPipeline().completedCount() == 1u, "cast completed");
    expectTrue(composer.castPipeline().instances()[0].state == fuse::fx::CastState::Done,
               "cast reached done state");

    fuse::fx::ResidualEntry residue;
    residue.kind = fuse::fx::ResidualKind::Zodiac;
    residue.assetId = "scorch_mark";
    residue.duration = 0.3f;
    residue.fadeDuration = 0.2f;
    composer.residuals().enqueue(residue);

    expectTrue(residualSpawns == 1u, "residual hook fired once");
    expectTrue(composer.residuals().activeCount() == 1u, "one active residual");

    composer.tick(ctx);
    expectTrue(composer.residuals().activeCount() == 1u, "residual still active mid-life");

    ctx.dt = 1.f;
    composer.tick(ctx);
    expectTrue(composer.residuals().activeCount() == 0u, "residual expired after fade");
    expectTrue(composer.residuals().totalExpired() == 1u, "residual expiry counted");
}

} // namespace

int main() {
    fuse::core::initialize();
    testRegisterDescriptors();
    testTickEffectAttachment();
    testCastPipelineAndResiduals();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_fx_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_fx_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
