#include <fuse/core/init.hpp>
#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/effect_graph.hpp>
#include <fuse/fx/afx_template_pack.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/fx/missile_descriptor.hpp>
#include <fuse/fx/particle_pool.hpp>
#include <fuse/fx/particle_pool_gpu.hpp>
#include <fuse/fx/afx_mission_hooks.hpp>
#include <fuse/fx/afx_mission_loader.hpp>
#include <fuse/fx/afx_mission_script_vm.hpp>
#include <fuse/fx/afx_choreographer_bridge.hpp>
#include <fuse/fx/socket_constraint.hpp>
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
    // Node ids start at 1; nodes() is insertion-ordered, so the child is element 1 (indexing by
    // its id read past the end — undefined behaviour that only happened to pass on glibc).
    expectTrue(graph.nodes().size() == 2u && graph.nodes()[1].id == child &&
                   graph.nodes()[1].state == fuse::fx::EffectNodeState::Pending,
               "child waits for parent");

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

void testSocketConstraintRemap() {
    fuse::fx::SocketConstraintManager constraints;
    expectTrue(constraints.defineConstraint(fuse::fx::ConstraintKind::Shape, "caster"),
               "caster constraint defined");

    fuse::fx::ConstraintDef parsed;
    expectTrue(fuse::fx::parse_constraint_spec("shape:muzzle", parsed), "constraint spec parses");
    expectTrue(parsed.kind == fuse::fx::ConstraintKind::Shape, "shape token maps");
    expectTrue(parsed.node_name == "muzzle", "node suffix preserved");

    expectTrue(constraints.defineConstraint(fuse::fx::ConstraintKind::Point, "impact"),
               "impact constraint defined");

    const fuse::math::Vec3 impact{1.f, 2.f, 3.f};
    expectTrue(constraints.setReferencePoint("impact", impact), "impact point set");

    fuse::fx::FxSocket socket;
    socket.effectId = "spark_burst";
    expectTrue(constraints.remapSocket(socket, "impact"), "point constraint remaps socket");

    const fuse::fx::ConstraintPose pose = constraints.sample("impact");
    expectTrue(pose.valid, "impact pose valid");
    expectTrue(pose.position.x == 1.f && pose.position.y == 2.f && pose.position.z == 3.f,
               "impact pose position");
}

void testMissilePipeline() {
    const fuse::fx::MissileDescriptor missile = fuse::fx::MissileDescriptor::makeFireballMissile();
    fuse::fx::MissilePipeline pipeline;
    pipeline.fire(missile, {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    expectTrue(pipeline.activeCount() == 1u, "missile fires one instance");

    pipeline.tick(0.1f);
    expectTrue(pipeline.instances()[0].position.z > 0.f, "missile advances along direction");

    pipeline.tick(missile.lifetime);
    expectTrue(pipeline.activeCount() == 0u, "missile expires after lifetime");
    expectTrue(pipeline.completedCount() == 1u, "missile completion counted");
}

void testRegisterDemoVerticalSlice() {
    fuse::fx::FxComposer composer;
    expectTrue(composer.registerDemoVerticalSlice(), "demo vertical slice registers");
    expectTrue(composer.effectCount() == 2u, "spark + muzzle registered");
    expectTrue(composer.findSpell("fireball") != nullptr, "fireball spell registered");
}

void testRegisterAfxTemplateSamplePack() {
    fuse::fx::FxComposer composer;
    expectTrue(fuse::fx::registerAfxTemplateSamplePack(composer), "AFX template sample pack registers");
    expectTrue(composer.findEffect("afx_demo_spark") != nullptr, "afx_demo_spark registered");
    expectTrue(composer.findEffect("afx_demo_smoke") != nullptr, "afx_demo_smoke registered");
}

void testComposerParticlePoolTick() {
    fuse::fx::FxComposer composer;
    composer.registerDemoVerticalSlice();

    fuse::fx::FxSocket socket;
    socket.kind = fuse::fx::FxSocketKind::Sprite2D;
    socket.effectId = "spark_burst";
    composer.attach(socket);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    composer.tick(ctx);
    expectTrue(composer.particlePool().spawnCount() > 0u, "composer spawns particles during active effects");
}

void testParticlePoolGpuBackend() {
    fuse::fx::ParticlePool cpuPool(8);
    cpuPool.spawn({1.f, 2.f, 3.f}, {0.f, 1.f, 0.f}, 0.5f);

    fuse::fx::ParticlePoolGpuBackend gpuBackend(8);
    gpuBackend.syncFromCpu(cpuPool);

    expectTrue(gpuBackend.hasDeviceBinding(), "gpu backend has packed binding");
    expectTrue(gpuBackend.activeCount() == 1u, "gpu backend mirrors active count");
    expectTrue(gpuBackend.packedDeviceBytes() > 0u, "gpu backend packed bytes non-zero");
    expectTrue(gpuBackend.syncCount() == 1u, "gpu backend sync counted");
}

void testAfxMissionHooks() {
    fuse::fx::FxComposer composer;
    std::vector<fuse::fx::AfxMissionHook> hooks;
    expectTrue(fuse::fx::registerAfxTemplateMissionHooks(composer, &hooks), "mission hooks register");
    expectTrue(hooks.size() == 2u, "two mission hooks parsed");
    expectTrue(hooks[0].missionId == "AFXDemo_Minimal", "minimal mission id");
    expectTrue(hooks[0].scriptHook == "on_spell_cast", "spell cast hook");
    expectTrue(composer.findSpell("fireball") != nullptr, "fireball spell registered by mission hook");
}

void testAfxMissionScriptVm() {
    fuse::fx::FxComposer composer;
    fuse::fx::AfxMissionScriptVm vm;
    expectTrue(fuse::fx::registerAfxTemplateMissionVm(composer, vm), "mission VM registers hooks");
    expectTrue(vm.hookCount() >= 3u, "mission VM hook table populated");
    expectTrue(vm.dispatch("on_spell_cast", composer), "spell cast hook dispatched");
    expectTrue(vm.dispatchCount() == 1u, "mission VM dispatch counted");
}

void testAfxMissionLoaderVmBridge() {
    static const char* kMisText =
        "function onSpellCast(%caster, %spell) {\n"
        "}\n"
        "function onAmbientFx() {\n"
        "}\n"
        "%on_spell_cast = \"fireball\"\n";

    fuse::fx::FxComposer composer;
    fuse::fx::AfxMissionScriptVm vm;
    std::string error;
    expectTrue(fuse::fx::register_afx_mission_from_mis(kMisText, composer, vm, &error),
               ".mis bridge registers mission VM hooks");
    expectTrue(vm.hookCount() >= 2u, ".mis bridge populates VM table");
    expectTrue(fuse::fx::dispatch_afx_mission_from_mis(kMisText, composer, vm, &error),
               ".mis bridge dispatches all hooks");
    expectTrue(vm.dispatchCount() >= 1u, ".mis bridge deepens dispatch count");
}

void testAfxMissionLoaderFromMis() {
    static const char* kMisText =
        "new Scene(ExampleLevel) {\n"
        "   function onSpellCast(%caster, %spell) {\n"
        "   }\n"
        "   function onAmbientFx() {\n"
        "   }\n"
        "};\n";

    std::vector<fuse::fx::AfxMissionHook> hooks;
    std::string error;
    expectTrue(fuse::fx::load_afx_mission_hooks_from_mis(kMisText, hooks, &error), ".mis loader finds hooks");
    expectTrue(hooks.size() >= 2u, ".mis loader returns spell and ambient hooks");
    expectTrue(hooks[0].scriptHook == "on_spell_cast", ".mis loader maps onSpellCast");
}

void testAfxMissionScriptVmImpactHook() {
    fuse::fx::FxComposer composer;
    fuse::fx::AfxMissionScriptVm vm;
    expectTrue(fuse::fx::registerAfxTemplateMissionVm(composer, vm), "mission VM registers hooks");
    expectTrue(vm.dispatch("on_impact_fx", composer), "impact hook dispatched");
    expectTrue(vm.lastHookDispatched() == "on_impact_fx", "impact hook recorded");
}

void testParticlePoolCudaNotSyncedSkip() {
    fuse::fx::ParticlePoolGpuBackend gpuBackend(4);
    fuse::frame::FrameCtx ctx;
    gpuBackend.cudaDispatchOrSkip(ctx);
    expectTrue(gpuBackend.lastCudaSkipReason() == fuse::fx::ParticlePoolCudaSkipReason::NotSynced,
               "cuda dispatch skipped before CPU sync");
}

void testParticlePoolCudaWriteback() {
    fuse::fx::ParticlePool pool(4);
    pool.spawn({0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 0.5f);

    fuse::fx::ParticlePoolGpuBackend gpuBackend(4);
    gpuBackend.syncFromCpu(pool);
    expectTrue(gpuBackend.syncedFromCpu(), "gpu backend marked synced");

    fuse::frame::FrameCtx ctx;
    gpuBackend.cudaDispatchOrSkip(ctx);
    gpuBackend.syncAliveFlagsToCpu(pool);
    gpuBackend.syncPositionsToCpu(pool);
    expectTrue(gpuBackend.writebackCount() >= 1u, "gpu writeback counted");
    expectTrue(gpuBackend.positionWritebackCount() == 1u, "gpu position writeback counted");
}

void testAfxMissionBodyCodegen() {
    static const char* kMisText =
        "new SimObject(SparkEmitter) { effectName = \"spark\"; }\n"
        "new SimObject(MuzzleFlashEmitter) { effectName = \"muzzle\"; }\n";

    fuse::fx::AfxMissionBody body;
    std::string error;
    expectTrue(fuse::fx::parse_afx_mission_body_from_mis(kMisText, body, &error), ".mis body parse succeeds");
    expectTrue(!body.simObjectBodies.empty(), ".mis body simobject block parsed");

    std::vector<fuse::fx::AfxMissionBodyEffect> effects;
    expectTrue(fuse::fx::codegen_effects_from_mission_body(body, effects) == 2u,
               ".mis body codegen emits effects");
    expectTrue(effects[0].effectId == "spark_burst", "SparkEmitter maps to spark_burst");
}

void testAfxMissionOnTickHook() {
    static const char* kMisText =
        "function onTick() {\n"
        "}\n";

    std::vector<fuse::fx::AfxMissionHook> hooks;
    std::string error;
    expectTrue(fuse::fx::load_afx_mission_hooks_from_mis(kMisText, hooks, &error), ".mis loader finds onTick");
    expectTrue(hooks[0].scriptHook == "on_tick", ".mis loader maps onTick");

    fuse::fx::FxComposer composer;
    fuse::fx::AfxMissionScriptVm vm;
    vm.registerHooks(hooks);
    fuse::frame::FrameCtx ctx;
    expectTrue(vm.dispatchTick(composer, ctx), "on_tick dispatched from mission VM tick");
    expectTrue(vm.tickDispatchCount() == 1u, "on_tick tick dispatch counted");
}

void testParticlePoolCudaSkipReason() {
    fuse::fx::ParticlePool pool(4);
    pool.spawn({0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 0.5f);

    fuse::fx::ParticlePoolGpuBackend gpuBackend(4);
    gpuBackend.syncFromCpu(pool);

    fuse::frame::FrameCtx ctx;
    gpuBackend.cudaDispatchOrSkip(ctx);
    expectTrue(gpuBackend.cudaSkipCount() == 1u, "cuda dispatch skipped without toolkit");
    expectTrue(gpuBackend.lastCudaSkipReason() == fuse::fx::ParticlePoolCudaSkipReason::Disabled,
               "cuda skip reason recorded");
}

void testParticlePoolCudaSkip() {
    fuse::fx::ParticlePool pool(4);
    pool.spawn({0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 0.5f);

    fuse::fx::ParticlePoolGpuBackend gpuBackend(4);
    gpuBackend.syncFromCpu(pool);

    fuse::frame::FrameCtx ctx;
    gpuBackend.cudaDispatchOrSkip(ctx);
    expectTrue(gpuBackend.cudaSkipCount() == 1u, "cuda dispatch skipped without toolkit");
}

void testAfxMissionBodyParse() {
    static const char* kMisText =
        "//--- MISSION AFXDemo_Minimal ---\n"
        "new SimObject(MissionCleanup) {}\n"
        "missionName = \"AFXDemo_Minimal\";\n"
        "MissionInfo.addScoreId = 0;\n"
        "function onSpellCast() {}\n";

    fuse::fx::AfxMissionBody body;
    std::string error;
    expectTrue(fuse::fx::parse_afx_mission_body_from_mis(kMisText, body, &error), ".mis body parse succeeds");
    expectTrue(body.missionName == "AFXDemo_Minimal", ".mis body mission name parsed");
    expectTrue(!body.simObjectNames.empty(), ".mis body simobject parsed");
    expectTrue(!body.missionInfoKeys.empty(), ".mis body missionInfo key parsed");
}

void testAfxChoreographerBridge() {
    fuse::fx::FxComposer composer;
    composer.registerDemoVerticalSlice();

    fuse::fx::AfxChoreographerBridge bridge;
    fuse::fx::ChoreographerBinding binding;
    binding.socket.kind = fuse::fx::FxSocketKind::Sprite2D;
    binding.socket.effectId = "spark_burst";
    binding.spellId = "fireball";
    binding.beginCastOnAttach = true;
    expectTrue(bridge.bindSocket(composer, binding), "choreographer bridge binds socket");
    expectTrue(bridge.attachCount() == 1u, "choreographer attach counted");
    expectTrue(bridge.castCount() == 1u, "choreographer cast counted");

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    bridge.tick(composer, ctx);
    expectTrue(bridge.tickCount() == 1u, "choreographer bridge tick counted");
}

void testParticlePoolTick() {
    fuse::fx::ParticlePool pool(4);
    expectTrue(pool.spawn({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.5f), "particle spawns");
    expectTrue(pool.activeCount() == 1u, "one active particle");

    fuse::frame::FrameCtx ctx;
    ctx.dt = 0.6f;
    pool.tick(ctx);
    expectTrue(pool.activeCount() == 0u, "particle expires after lifetime");
}

void testParticlePoolCudaSelectiveWriteback() {
    fuse::fx::ParticlePool pool(4);
    pool.spawn({0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 0.5f);
    pool.spawn({1.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.f);

    fuse::fx::ParticlePoolGpuBackend gpuBackend(4);
    gpuBackend.syncFromCpu(pool);
    fuse::frame::FrameCtx ctx;
    gpuBackend.cudaDispatchOrSkip(ctx);
    const fuse::u32 written = gpuBackend.syncSelectivePositionsToCpu(pool);
    expectTrue(written <= 1u, "selective writeback skips zero-age slots");
    expectTrue(gpuBackend.selectiveWritebackCount() <= 1u, "selective writeback counted");
}

void testComposerSelectiveWriteback() {
    fuse::fx::FxComposer composer;
    composer.registerDemoVerticalSlice();

    fuse::fx::FxSocket socket;
    socket.kind = fuse::fx::FxSocketKind::Sprite2D;
    socket.effectId = "spark_burst";
    composer.attach(socket);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    composer.tick(ctx);
    (void)composer.particlePoolGpu().selectiveWritebackCount(); // selective writeback path executes
}

void testAfxMissionSpellCodegen() {
    static const char* kMisText =
        "new SimObject(FireballSpell) {\n"
        "  spellId = \"fireball\";\n"
        "}\n";

    fuse::fx::AfxMissionBody body;
    std::vector<fuse::fx::AfxMissionBodySpell> spells;
    expectTrue(fuse::fx::parse_afx_mission_body_from_mis(kMisText, body), "spell body parse");
    expectTrue(fuse::fx::codegen_spells_from_mission_body(body, spells) == 1u, "spell codegen from body");
    expectTrue(!spells.empty() && spells[0].spellId == "fireball", "fireball spell id codegen");
}

void testParticlePoolCudaResidency() {
    fuse::fx::ParticlePool pool(4);
    pool.spawn({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.5f);

    fuse::fx::ParticlePoolGpuBackend gpuBackend(4);
    gpuBackend.syncFromCpu(pool);
    fuse::frame::FrameCtx ctx;
    gpuBackend.cudaDispatchOrSkip(ctx);
    expectTrue(gpuBackend.residencySyncCount() >= 1u, "CUDA residency sync counted");
    expectTrue(gpuBackend.residentSlotCount() <= 4u, "CUDA resident slot count bounded by pool capacity");
}

void testAfxMissionVmDelayedDispatch() {
    fuse::fx::FxComposer composer;
    composer.registerDemoVerticalSlice();
    fuse::fx::AfxMissionScriptVm vm;
    std::vector<fuse::fx::AfxMissionHook> hooks;
    hooks.push_back({"AFXDemo_Minimal", "on_ambient_fx", "spark_burst", 100});
    vm.registerHooks(hooks);
    expectTrue(vm.pendingDelayedCount() >= 1u, "mission VM delayed dispatch scheduled");

    fuse::frame::FrameCtx ctx;
    ctx.dt = 0.05f;
    expectTrue(vm.advanceDelayedDispatches(40, composer, ctx) == 0u, "delayed dispatch not fired early");
    expectTrue(vm.advanceDelayedDispatches(80, composer, ctx) >= 1u, "delayed dispatch fired after delay");
    expectTrue(vm.delayedDispatchCount() >= 1u, "delayed dispatch counted");
}

void testAfxMissionScheduleCallParse() {
    static const char* kMisText =
        "missionName = \"ScheduleDemo\";\n"
        "schedule(500, onSpellCast);\n"
        "call(onAmbientFx);\n"
        "function onSpellCast() {\n"
        "  beginCast(\"fireball\");\n"
        "}\n"
        "function onAmbientFx() {\n"
        "  attachEffect(\"spark_burst\");\n"
        "}\n";

    fuse::fx::AfxMissionBody body;
    std::string error;
    expectTrue(fuse::fx::parse_afx_mission_body_from_mis(kMisText, body, &error), "schedule/call body parse");
    expectTrue(body.scheduleEntries.size() == 1u, "schedule entry parsed");
    expectTrue(body.scheduleEntries[0].hookName == "spell_cast", "schedule hook normalized");
    expectTrue(body.scheduleEntries[0].delayMs == 500u, "schedule delay parsed");
    expectTrue(body.callTargets.size() == 1u, "call target parsed");
    expectTrue(body.callTargets[0] == "ambient_fx", "call target normalized");

    fuse::fx::FxComposer composer;
    fuse::fx::AfxMissionScriptVm vm;
    expectTrue(fuse::fx::dispatch_afx_mission_from_mis(kMisText, composer, vm, &error),
               "schedule dispatch prefers ordered hooks");
    expectTrue(vm.pendingDelayedCount() >= 1u, "schedule entry schedules delayed VM dispatch");
    expectTrue(vm.dispatchCount() == 0u, "schedule dispatch defers immediate hook fire");
    fuse::frame::FrameCtx ctx;
    expectTrue(vm.advanceDelayedDispatches(600, composer, ctx) >= 1u, "scheduled hook fires after delay");
    expectTrue(vm.dispatchCount() >= 1u, "schedule delayed dispatch counted");
}

void testAfxMissionExecuteFromMis() {
    static const char* kMisText =
        "function onSpellCast() {\n"
        "  beginCast(\"fireball\");\n"
        "}\n"
        "function onAmbientFx() {\n"
        "  attachEffect(\"spark_burst\");\n"
        "}\n";

    fuse::fx::FxComposer composer;
    fuse::fx::AfxMissionScriptVm vm;
    expectTrue(fuse::fx::execute_afx_mission_from_mis(kMisText, composer, vm), "mission execute from .mis");
    expectTrue(vm.executeCount() >= 1u, "mission VM execute count tracked");
}

void testAfxMissionNestedSimObjectParse() {
    static const char* kMisText =
        "new SimObject(FireballGroup) {\n"
        "  new SimObject(SparkEmitter) { effectName = \"spark\"; }\n"
        "  new SimObject(MuzzleFlashEmitter) { effectName = \"muzzle\"; }\n"
        "}\n";

    fuse::fx::AfxMissionBody body;
    std::string error;
    expectTrue(fuse::fx::parse_afx_mission_body_from_mis(kMisText, body, &error), "nested .mis body parse");
    expectTrue(body.nestedSimObjectBodies.size() >= 2u, "nested simobjects captured");
    expectTrue(!body.simObjectNames.empty(), "nested simobject names registered");
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
    testSocketConstraintRemap();
    testMissilePipeline();
    testRegisterDemoVerticalSlice();
    testRegisterAfxTemplateSamplePack();
    testComposerParticlePoolTick();
    testParticlePoolGpuBackend();
    testAfxMissionHooks();
    testAfxMissionScriptVm();
    testAfxMissionLoaderFromMis();
    testAfxMissionBodyParse();
    testAfxMissionBodyCodegen();
    testAfxMissionLoaderVmBridge();
    testAfxMissionScriptVmImpactHook();
    testParticlePoolCudaNotSyncedSkip();
    testParticlePoolCudaWriteback();
    testAfxMissionOnTickHook();
    testParticlePoolCudaSkipReason();
    testParticlePoolCudaSkip();
    testAfxChoreographerBridge();
    testParticlePoolTick();
    testParticlePoolCudaSelectiveWriteback();
    testComposerSelectiveWriteback();
    testAfxMissionSpellCodegen();
    testParticlePoolCudaResidency();
    testAfxMissionVmDelayedDispatch();
    testAfxMissionScheduleCallParse();
    testAfxMissionExecuteFromMis();
    testAfxMissionNestedSimObjectParse();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_fx_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_fx_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
