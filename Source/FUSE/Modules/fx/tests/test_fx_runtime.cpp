#include <fuse/core/init.hpp>
#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/effect_graph.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/fx/fx_defs.hpp>
#include <fuse/fx/parameter_bind.hpp>
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
    const fuse::fx::EffectDescriptor muzzle = fuse::fx::EffectDescriptor::makeMuzzleFlash();
    const fuse::fx::SpellDescriptor fireball = fuse::fx::SpellDescriptor::makeFireball();

    expectTrue(composer.registerEffect(spark), "spark effect registers");
    expectTrue(composer.registerEffect(muzzle), "muzzle flash effect registers");
    expectTrue(composer.registerSpell(fireball), "fireball spell registers");
    expectTrue(composer.effectCount() == 2u, "two effect descriptors");
    expectTrue(composer.spellCount() == 1u, "one spell descriptor");
    expectTrue(composer.findEffect("spark_burst") != nullptr, "spark effect lookup");
    expectTrue(composer.findEffect("muzzle_flash") != nullptr, "muzzle flash effect lookup");
    expectTrue(composer.findSpell("fireball") != nullptr, "fireball spell lookup");
}

void testSocketAttachAndTimeline() {
    fuse::fx::FxComposer composer;
    composer.registerEffect(fuse::fx::EffectDescriptor::makeSparkBurst());
    composer.registerEffect(fuse::fx::EffectDescriptor::makeMuzzleFlash());

    fuse::fx::FxSocket spriteSocket;
    spriteSocket.kind = fuse::fx::FxSocketKind::Sprite2D;
    spriteSocket.owner = fuse::Handle<fuse::Object>(10, 1);
    spriteSocket.effectId = "spark_burst";
    expectTrue(composer.attach(spriteSocket), "sprite socket attaches");

    fuse::fx::FxSocket shapeSocket;
    shapeSocket.kind = fuse::fx::FxSocketKind::Shape3D;
    shapeSocket.owner = fuse::Handle<fuse::Object>(11, 1);
    shapeSocket.effectId = "muzzle_flash";
    expectTrue(composer.attach(shapeSocket), "shape socket attaches");

    fuse::fx::FxSocket badSocket;
    badSocket.effectId = "unknown_effect";
    expectTrue(!composer.attach(badSocket), "unknown effect rejected");

    expectTrue(composer.attachmentCount() == 2u, "two sockets attached");
    expectTrue(composer.sockets().size() == 2u, "socket list size");
    expectTrue(composer.effectTimeline().activeCount() == 2u, "two active effect playbacks");

    fuse::frame::FrameCtx ctx;
    ctx.dt = 0.2f;

    while (composer.effectTimeline().activeCount() > 0) {
        composer.tick(ctx);
    }

    expectTrue(composer.effectTimeline().completedCount() == 2u, "both effects completed");
    expectTrue(composer.effectTimeline().instances()[0].state == fuse::fx::EffectPlaybackState::Done,
               "first playback done");
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
    expectTrue(residualSpawns == 1u, "impact phase spawned one residual");
    expectTrue(composer.residuals().activeCount() == 1u, "one active residual from cast");

    fuse::fx::ResidualEntry residue;
    residue.kind = fuse::fx::ResidualKind::Zodiac;
    residue.assetId = "scorch_mark";
    residue.duration = 0.3f;
    residue.fadeDuration = 0.2f;
    composer.residuals().enqueue(residue);

    expectTrue(residualSpawns == 2u, "manual residual hook fired");
    expectTrue(composer.residuals().activeCount() == 2u, "two active residuals");

    ctx.dt = 0.6f;
    composer.tick(ctx);
    expectTrue(composer.residuals().activeCount() == 1u, "short residual expired first");

    ctx.dt = 3.f;
    composer.tick(ctx);
    expectTrue(composer.residuals().activeCount() == 0u, "cast residual expired after full lifetime");
    expectTrue(composer.residuals().totalExpired() == 2u, "residual expiry counted");
}

void testEffectGraphParentChildTick() {
    fuse::fx::FxComposer composer;
    composer.registerEffect(fuse::fx::EffectDescriptor::makeSparkBurst());
    composer.registerEffect(fuse::fx::EffectDescriptor::makeMuzzleFlash());

    fuse::fx::EffectGraph& graph = composer.effectGraph();
    const fuse::u32 parent = graph.addNode("spark_burst");
    const fuse::u32 child = graph.addNode("muzzle_flash", parent);
    graph.activate();

    expectTrue(graph.activated(), "graph activated");
    expectTrue(graph.activeCount() == 1u, "only root active initially");
    expectTrue(graph.nodes()[1].state == fuse::fx::EffectNodeState::Pending, "child waits for parent");

    fuse::frame::FrameCtx ctx;
    ctx.dt = 0.2f;
    composer.tick(ctx);
    expectTrue(graph.nodes()[0].state == fuse::fx::EffectNodeState::Active, "parent still running");
    expectTrue(graph.nodes()[1].state == fuse::fx::EffectNodeState::Pending, "child still pending");

    ctx.dt = 0.16f;
    composer.tick(ctx);
    expectTrue(graph.nodes()[0].state == fuse::fx::EffectNodeState::Done, "parent completed");
    expectTrue(graph.nodes()[1].state == fuse::fx::EffectNodeState::Active, "child activated");
    expectTrue(graph.activeCount() == 1u, "child now sole active node");

    ctx.dt = 0.15f;
    composer.tick(ctx);
    expectTrue(graph.completedCount() == 2u, "parent and child completed");
    expectTrue(graph.activeCount() == 0u, "no active nodes remain");
    expectTrue(graph.tickCount() == 3u, "graph tick count advanced");
}

void testEffectGraphParallelRoots() {
    fuse::fx::FxComposer composer;
    composer.registerEffect(fuse::fx::EffectDescriptor::makeSparkBurst());
    composer.registerEffect(fuse::fx::EffectDescriptor::makeMuzzleFlash());

    fuse::fx::EffectGraph& graph = composer.effectGraph();
    graph.addNode("spark_burst");
    graph.addNode("muzzle_flash");
    graph.activate();

    expectTrue(graph.activeCount() == 2u, "both roots active");

    fuse::frame::FrameCtx ctx;
    ctx.dt = 0.5f;
    composer.tick(ctx);

    expectTrue(graph.completedCount() == 2u, "parallel roots completed in one tick window");
    expectTrue(graph.activeCount() == 0u, "graph drained");
}

void testParameterBindCastResolution() {
    fuse::fx::bind::ParameterBinder binder;
    const fuse::Handle<fuse::Object> caster(42, 1);
    const fuse::Handle<fuse::Object> target(77, 2);

    binder.bindHandle("caster", caster);
    binder.bindHandle("target", target);
    binder.bindFloat("intensity", 1.5f);

    expectTrue(binder.has("caster"), "caster slot bound");
    expectTrue(binder.count() == 3u, "three parameters bound");

    const fuse::fx::CastBinding resolved = binder.resolveCastBinding();
    expectTrue(resolved.caster == caster, "caster handle resolved");
    expectTrue(resolved.target == target, "target handle resolved");

    const fuse::fx::bind::ParameterValue* intensity = binder.get("intensity");
    expectTrue(intensity != nullptr, "intensity slot exists");
    expectTrue(intensity->kind == fuse::fx::bind::ParameterKind::Float, "intensity is float");
    expectTrue(intensity->floatValue == 1.5f, "intensity value preserved");
}

void testParameterBindComposerCast() {
    fuse::fx::FxComposer composer;
    composer.registerSpell(fuse::fx::SpellDescriptor::makeFireball());

    composer.parameters().bindHandle("caster", fuse::Handle<fuse::Object>(3, 1));
    composer.parameters().bindHandle("target", fuse::Handle<fuse::Object>(4, 1));

    expectTrue(composer.beginCast("fireball", composer.parameters().resolveCastBinding()),
               "bound cast begins");
    expectTrue(composer.castPipeline().instances()[0].binding.caster.index() == 3u,
               "cast uses bound caster");
    expectTrue(composer.castPipeline().instances()[0].binding.target.index() == 4u,
               "cast uses bound target");
}

void testFireballPhaseProgression() {
    fuse::fx::FxComposer composer;
    composer.registerSpell(fuse::fx::SpellDescriptor::makeFireball());

    fuse::fx::CastBinding binding;
    binding.caster = fuse::Handle<fuse::Object>(3, 1);
    expectTrue(composer.beginCast("fireball", binding), "fireball cast begins");

    fuse::frame::FrameCtx ctx;
    ctx.dt = 0.1f;

    composer.tick(ctx);
    expectTrue(composer.castPipeline().instances()[0].phase == fuse::fx::SpellPhase::Casting,
               "starts in casting phase");

    ctx.dt = 0.3f;
    composer.tick(ctx);
    expectTrue(composer.castPipeline().instances()[0].phase == fuse::fx::SpellPhase::Casting,
               "still casting before duration elapses");

    ctx.dt = 0.2f;
    composer.tick(ctx);
    expectTrue(composer.castPipeline().instances()[0].phase == fuse::fx::SpellPhase::Delivery,
               "instant launch phase advances to delivery");
}

} // namespace

int main() {
    fuse::core::initialize();
    testRegisterDescriptors();
    testSocketAttachAndTimeline();
    testEffectGraphParentChildTick();
    testEffectGraphParallelRoots();
    testParameterBindCastResolution();
    testParameterBindComposerCast();
    testCastPipelineAndResiduals();
    testFireballPhaseProgression();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_fx_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_fx_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
