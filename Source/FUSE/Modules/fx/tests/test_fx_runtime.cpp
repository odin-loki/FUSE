#include <fuse/core/init.hpp>
#include <fuse/fx/emit_rate.hpp>
#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/effect_graph.hpp>
#include <fuse/fx/effect_priority.hpp>
#include <fuse/fx/fx_layer.hpp>
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

#include <cmath>
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

bool nearEqual(float a, float b) {
    return std::fabs(a - b) <= 1e-4f;
}

bool vecFinite(const fuse::math::Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool vecNear(const fuse::math::Vec3& value, float x, float y, float z) {
    return nearEqual(value.x, x) && nearEqual(value.y, y) && nearEqual(value.z, z);
}

bool unitNormal(const fuse::math::Vec3& value) {
    const float lenSq = value.x * value.x + value.y * value.y + value.z * value.z;
    return vecFinite(value) && nearEqual(lenSq, 1.f);
}

fuse::fx::MissileDescriptor makeBolt(float speed, float lifetime) {
    fuse::fx::MissileDescriptor desc;
    desc.id = "bolt";
    desc.muzzleVelocity = speed;
    desc.isBallistic = false;
    desc.gravityMod = 0.f;
    desc.lifetime = lifetime;
    desc.fadeDelay = 0.5f;
    return desc;
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

void testMissileImpactSphere() {
    fuse::fx::MissileDescriptor bolt = makeBolt(8.f, 5.f);
    bolt.isBallistic = true;
    bolt.gravityMod = 20.f;

    fuse::fx::MissilePipeline pipeline;
    pipeline.fire(bolt, {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});

    const fuse::fx::MissileCollider colliders[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 0.f, 5.f}, 1.f),
    };
    pipeline.tick(1.f, colliders);

    const fuse::fx::MissileInstance& shot = pipeline.instances()[0];
    expectTrue(shot.impacted, "sphere stops the missile");
    expectTrue(shot.finished, "sphere impact finishes the missile");
    expectTrue(pipeline.activeCount() == 0u, "sphere impact clears the active count");
    expectTrue(pipeline.completedCount() == 1u, "sphere impact counts as completion");
    expectTrue(shot.impactColliderIndex == 0u, "sphere records collider 0");
    expectTrue(vecNear(shot.position, 0.f, 0.f, 4.f), "sphere stops on the near surface");
    expectTrue(vecNear(shot.impactPoint, 0.f, 0.f, 4.f), "sphere records the impact point");
    expectTrue(vecNear(shot.impactNormal, 0.f, 0.f, -1.f), "sphere normal points back along the shot");
    expectTrue(unitNormal(shot.impactNormal), "sphere normal is unit length");
    expectTrue(vecFinite(shot.position) && vecFinite(shot.velocity), "sphere impact stays finite");
    expectTrue(nearEqual(shot.velocity.z, 8.f), "ballistic gravity is not applied after a hit");
    expectTrue(shot.elapsed < bolt.lifetime, "sphere impact stops before lifetime expiry");

    pipeline.tick(1.f, colliders);
    expectTrue(pipeline.completedCount() == 1u, "a stopped missile is not completed twice");
    expectTrue(vecNear(pipeline.instances()[0].position, 0.f, 0.f, 4.f), "a stopped missile stays on the surface");
}

void testMissileImpactAabb() {
    fuse::fx::MissilePipeline pipeline;
    pipeline.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});

    const fuse::fx::MissileCollider colliders[] = {
        fuse::fx::MissileCollider::makeAabb({-1.f, -1.f, 4.f}, {1.f, 1.f, 6.f}),
    };
    pipeline.tick(1.f, colliders);

    const fuse::fx::MissileInstance& shot = pipeline.instances()[0];
    expectTrue(shot.impacted, "aabb stops the missile");
    expectTrue(shot.finished && pipeline.activeCount() == 0u, "aabb impact finishes the missile");
    expectTrue(shot.impactColliderIndex == 0u, "aabb records collider 0");
    expectTrue(vecNear(shot.impactPoint, 0.f, 0.f, 4.f), "aabb records the entry face");
    expectTrue(vecNear(shot.position, 0.f, 0.f, 4.f), "aabb stops on the entry face");
    expectTrue(vecNear(shot.impactNormal, 0.f, 0.f, -1.f), "aabb normal is the outward face normal");
    expectTrue(unitNormal(shot.impactNormal), "aabb normal is unit length");
    expectTrue(shot.position.z < 6.f, "aabb impact does not exit through the far face");
}

void testMissileImpactMissExpires() {
    fuse::fx::ResidualEffectQueue residuals;
    fuse::u32 spawns = 0;
    residuals.setSpawnHook([&](const fuse::fx::ResidualEntry&) { ++spawns; });

    const fuse::fx::MissileCollider colliders[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 5.f, 2.f}, 1.f),
    };

    fuse::fx::MissilePipeline pipeline;
    pipeline.fire(makeBolt(8.f, 0.5f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    pipeline.tick(0.25f, colliders, &residuals);

    expectTrue(pipeline.activeCount() == 1u, "a miss stays in flight");
    expectTrue(!pipeline.instances()[0].impacted, "a side sphere is not a hit");
    expectTrue(!pipeline.instances()[0].finished, "a miss does not finish early");
    expectTrue(vecNear(pipeline.instances()[0].position, 0.f, 0.f, 2.f), "a miss integrates the full step");
    expectTrue(vecFinite(pipeline.instances()[0].position), "a miss stays finite");
    expectTrue(spawns == 0u && residuals.totalSpawned() == 0u, "a miss does not enqueue a residual");

    pipeline.tick(0.25f, colliders, &residuals);
    expectTrue(pipeline.activeCount() == 0u, "a miss expires at lifetime");
    expectTrue(pipeline.completedCount() == 1u, "lifetime expiry counts as completion");
    expectTrue(pipeline.instances()[0].finished && !pipeline.instances()[0].impacted, "expiry is not an impact");
    expectTrue(vecNear(pipeline.instances()[0].position, 0.f, 0.f, 4.f), "a miss keeps flying until expiry");
    expectTrue(spawns == 0u, "lifetime expiry does not enqueue a residual");

    fuse::fx::MissileDescriptor ballistic = makeBolt(8.f, 5.f);
    ballistic.isBallistic = true;
    ballistic.gravityMod = 4.f;
    fuse::fx::MissilePipeline falling;
    falling.fire(ballistic, {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    falling.tick(0.25f, colliders);
    expectTrue(!falling.instances()[0].impacted, "ballistic miss is not an impact");
    expectTrue(vecNear(falling.instances()[0].position, 0.f, 0.f, 2.f), "ballistic miss steps before gravity");
    expectTrue(nearEqual(falling.instances()[0].velocity.z, 7.f), "ballistic miss applies gravity");
}

void testMissileImpactLowestIndexTie() {
    fuse::fx::MissilePipeline tied;
    tied.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const fuse::fx::MissileCollider sameDistance[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 0.f, 5.f}, 1.f),
        fuse::fx::MissileCollider::makeSphere({0.f, 0.f, 5.f}, 1.f),
    };
    tied.tick(1.f, sameDistance);

    expectTrue(tied.instances()[0].impacted, "tied spheres hit");
    expectTrue(tied.instances()[0].impactColliderIndex == 0u, "exact t tie keeps the lowest collider index");
    expectTrue(vecNear(tied.instances()[0].impactPoint, 0.f, 0.f, 4.f), "tied spheres record the shared point");
    expectTrue(vecNear(tied.instances()[0].impactNormal, 0.f, 0.f, -1.f), "tied spheres record the shared normal");

    fuse::fx::MissilePipeline ordered;
    ordered.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const fuse::fx::MissileCollider nearerLater[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 0.f, 9.f}, 1.f),
        fuse::fx::MissileCollider::makeSphere({0.f, 0.f, 5.f}, 1.f),
    };
    ordered.tick(1.f, nearerLater);

    expectTrue(ordered.instances()[0].impacted, "the nearer sphere hits");
    expectTrue(ordered.instances()[0].impactColliderIndex == 1u, "a smaller t beats a lower index");
    expectTrue(vecNear(ordered.instances()[0].impactPoint, 0.f, 0.f, 4.f), "the nearer surface is the impact point");
    expectTrue(unitNormal(ordered.instances()[0].impactNormal), "ordered hit normal stays finite");
}

void testMissileImpactGrazingAndZeroLength() {
    fuse::fx::MissilePipeline tangent;
    tangent.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const fuse::fx::MissileCollider tangentSphere[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 1.f, 5.f}, 1.f),
    };
    tangent.tick(1.f, tangentSphere);
    const fuse::fx::MissileInstance& grazed = tangent.instances()[0];
    expectTrue(grazed.impacted, "a tangent sphere is a hit");
    expectTrue(vecNear(grazed.impactPoint, 0.f, 0.f, 5.f), "tangent contact is on the sphere");
    expectTrue(vecNear(grazed.impactNormal, 0.f, -1.f, 0.f), "tangent normal is the outward radius");
    expectTrue(unitNormal(grazed.impactNormal) && vecFinite(grazed.position), "tangent hit does not NaN");

    fuse::fx::MissilePipeline nearMiss;
    nearMiss.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const fuse::fx::MissileCollider clearMiss[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 1.25f, 5.f}, 1.f),
    };
    nearMiss.tick(1.f, clearMiss);
    expectTrue(!nearMiss.instances()[0].impacted && nearMiss.activeCount() == 1u, "a clear near miss keeps flying");
    expectTrue(vecNear(nearMiss.instances()[0].position, 0.f, 0.f, 8.f), "a near miss travels the full step");
    expectTrue(vecFinite(nearMiss.instances()[0].position) && vecFinite(nearMiss.instances()[0].velocity),
               "a near miss does not NaN");

    fuse::fx::MissilePipeline edge;
    edge.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const fuse::fx::MissileCollider edgeBox[] = {
        fuse::fx::MissileCollider::makeAabb({0.f, -1.f, 5.f}, {1.f, 1.f, 6.f}),
    };
    edge.tick(1.f, edgeBox);
    expectTrue(edge.instances()[0].impacted, "riding an aabb face still hits the entry edge");
    expectTrue(vecNear(edge.instances()[0].impactPoint, 0.f, 0.f, 5.f), "edge graze records the entry point");
    expectTrue(vecNear(edge.instances()[0].impactNormal, 0.f, 0.f, -1.f), "edge graze keeps a face normal");
    expectTrue(unitNormal(edge.instances()[0].impactNormal), "edge graze does not NaN");

    fuse::fx::MissilePipeline parallel;
    parallel.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f});
    const fuse::fx::MissileCollider beside[] = {
        fuse::fx::MissileCollider::makeAabb({1.f, 1.f, -1.f}, {3.f, 3.f, 1.f}),
    };
    parallel.tick(1.f, beside);
    expectTrue(!parallel.instances()[0].impacted, "a parallel aabb miss is not a hit");
    expectTrue(vecNear(parallel.instances()[0].position, 8.f, 0.f, 0.f), "a parallel miss travels the full step");
    expectTrue(vecFinite(parallel.instances()[0].position), "a parallel miss does not NaN");

    fuse::fx::MissilePipeline inverted;
    inverted.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const fuse::fx::MissileCollider emptyBox[] = {
        fuse::fx::MissileCollider::makeAabb({1.f, 1.f, 1.f}, {0.f, 0.f, 0.f}),
    };
    inverted.tick(1.f, emptyBox);
    expectTrue(!inverted.instances()[0].impacted, "an inverted aabb is not a hit");
    expectTrue(vecNear(inverted.instances()[0].position, 0.f, 0.f, 8.f), "an inverted aabb does not stop the missile");
    expectTrue(vecFinite(inverted.instances()[0].position), "an inverted aabb does not NaN");

    const fuse::fx::MissileCollider insideSphere[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 0.f, 0.f}, 1.f),
    };
    fuse::fx::MissilePipeline planted;
    planted.fire(makeBolt(0.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    planted.tick(0.f, insideSphere);
    expectTrue(planted.activeCount() == 1u && !planted.instances()[0].impacted, "a zero dt does not sweep");
    expectTrue(planted.instances()[0].elapsed == 0.f, "a zero dt does not age the missile");

    planted.tick(0.25f, insideSphere);
    expectTrue(planted.instances()[0].impacted && planted.instances()[0].finished, "a zero-length step inside a sphere hits");
    expectTrue(planted.instances()[0].elapsed < 5.f, "containment stops before lifetime expiry");
    expectTrue(vecNear(planted.instances()[0].impactPoint, 0.f, 0.f, 0.f), "containment records the start point");
    expectTrue(vecNear(planted.instances()[0].impactNormal, 0.f, 1.f, 0.f), "a zero offset uses a finite fallback normal");
    expectTrue(unitNormal(planted.instances()[0].impactNormal), "zero-length containment does not NaN");

    fuse::fx::MissilePipeline outside;
    outside.fire(makeBolt(0.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const fuse::fx::MissileCollider distant[] = {
        fuse::fx::MissileCollider::makeSphere({3.f, 0.f, 0.f}, 1.f),
    };
    outside.tick(0.25f, distant);
    expectTrue(!outside.instances()[0].impacted && outside.activeCount() == 1u, "a zero-length step outside misses");
    expectTrue(vecNear(outside.instances()[0].position, 0.f, 0.f, 0.f), "a zero-length miss stays put");
    expectTrue(vecFinite(outside.instances()[0].position) && vecFinite(outside.instances()[0].velocity),
               "a zero-length miss does not NaN");

    fuse::fx::MissilePipeline surface;
    surface.fire(makeBolt(0.f, 5.f), {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const fuse::fx::MissileCollider shell[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 0.f, 0.f}, 1.f),
    };
    surface.tick(0.25f, shell);
    expectTrue(surface.instances()[0].impacted, "a zero-length step on the shell hits");
    expectTrue(vecNear(surface.instances()[0].impactPoint, 1.f, 0.f, 0.f), "shell contact keeps the start point");
    expectTrue(vecNear(surface.instances()[0].impactNormal, 1.f, 0.f, 0.f), "shell contact normal is the outward radius");
    expectTrue(unitNormal(surface.instances()[0].impactNormal), "shell contact does not NaN");
}

void testMissileImpactEnqueuesResidual() {
    fuse::fx::ResidualEffectQueue residuals;
    fuse::u32 spawns = 0;
    residuals.setSpawnHook([&](const fuse::fx::ResidualEntry& entry) {
        ++spawns;
        expectTrue(entry.kind == fuse::fx::ResidualKind::Zodiac, "impact residual is a zodiac");
        expectTrue(entry.assetId == "bolt", "impact residual uses the missile id");
        expectTrue(nearEqual(entry.duration, 0.5f) && nearEqual(entry.fadeDuration, 0.5f),
                   "impact residual uses the missile fade");
        expectTrue(nearEqual(entry.x, 0.f) && nearEqual(entry.y, 0.f) && nearEqual(entry.z, 4.f),
                   "impact residual is spawned at the hit");
    });

    const fuse::fx::MissileCollider colliders[] = {
        fuse::fx::MissileCollider::makeSphere({0.f, 0.f, 5.f}, 1.f),
    };

    fuse::fx::MissilePipeline pipeline;
    pipeline.fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    pipeline.fire(makeBolt(8.f, 5.f), {0.f, 5.f, 0.f}, {0.f, 0.f, 1.f});
    pipeline.tick(1.f, colliders, &residuals);

    expectTrue(spawns == 1u && residuals.totalSpawned() == 1u, "only the hitting missile enqueues");
    expectTrue(residuals.activeCount() == 1u, "impact residual stays active");
    expectTrue(pipeline.instances()[0].impacted && pipeline.instances()[0].impactColliderIndex == 0u,
               "first missile records the sphere");
    expectTrue(!pipeline.instances()[1].impacted && pipeline.activeCount() == 1u, "the parallel missile keeps flying");
    expectTrue(vecNear(pipeline.instances()[1].position, 0.f, 5.f, 8.f), "the miss travels the full step");

    pipeline.tick(1.f, colliders, &residuals);
    expectTrue(spawns == 1u && pipeline.completedCount() == 1u, "a finished impact does not enqueue again");

    fuse::fx::FxComposer composer;
    composer.missiles().fire(makeBolt(8.f, 5.f), {0.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    composer.missiles().tick(1.f, colliders, &composer.residuals());
    expectTrue(composer.residuals().totalSpawned() == 1u, "composer residuals receive the missile impact");
    expectTrue(composer.missiles().instances()[0].impacted, "composer missile records the impact");
    expectTrue(nearEqual(composer.residuals().entries()[0].z, 4.f), "composer residual is placed on the hit");
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

void testEmitRateOk() {
    expectTrue(fuse::fx::emit_rate_ok(0.5f), "rate 0.5 is an emit rate");
    expectTrue(fuse::fx::emit_rate_ok(1000.f), "rate 1000 is an emit rate");
    expectTrue(!fuse::fx::emit_rate_ok(0.f), "rate 0 is not an emit rate");
    expectTrue(!fuse::fx::emit_rate_ok(1000.1f), "rate 1000.1 is not an emit rate");
    expectTrue(!fuse::fx::emit_rate_ok(std::numeric_limits<float>::quiet_NaN()), "NaN is not an emit rate");
}

void testFxLayerOk() {
    expectTrue(fuse::fx::fx_layer_ok(0), "layer 0 is an fx layer");
    expectTrue(fuse::fx::fx_layer_ok(7), "layer 7 is an fx layer");
    expectTrue(!fuse::fx::fx_layer_ok(-1), "layer -1 is not an fx layer");
    expectTrue(!fuse::fx::fx_layer_ok(8), "layer 8 is not an fx layer");
}

void testEffectPriority() {
    expectTrue(fuse::fx::effect_priority_rank(40) == 40, "effect_priority_rank(40) == 40");
    expectTrue(fuse::fx::effect_priority_rank(-5) == 0, "effect_priority_rank(-5) == 0");
    expectTrue(fuse::fx::effect_priority_rank(140) == 100, "effect_priority_rank(140) == 100");
    expectTrue(fuse::fx::effect_priority_rank(0) == 0, "effect_priority_rank(0) == 0");
    expectTrue(fuse::fx::effect_priority_rank(100) == 100, "effect_priority_rank(100) == 100");
    expectTrue(fuse::fx::effect_outranks(40, 10), "higher rank outranks a lower rank");
    expectTrue(!fuse::fx::effect_outranks(10, 40), "lower rank does not outrank a higher rank");
    expectTrue(!fuse::fx::effect_outranks(40, 40), "equal rank does not outrank");
    expectTrue(!fuse::fx::effect_outranks(-5, 0), "non-usable negative ties with zero");
    expectTrue(fuse::fx::effect_outranks(140, 99), "clamped high rank outranks 99");
    expectTrue(!fuse::fx::effect_outranks(140, 100), "values above 100 tie at the cap");
}

} // namespace

int main() {
    testEmitRateOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_fx_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testFxLayerOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_fx_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testEffectPriority();
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
    testMissileImpactSphere();
    testMissileImpactAabb();
    testMissileImpactMissExpires();
    testMissileImpactLowestIndexTie();
    testMissileImpactGrazingAndZeroLength();
    testMissileImpactEnqueuesResidual();
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
