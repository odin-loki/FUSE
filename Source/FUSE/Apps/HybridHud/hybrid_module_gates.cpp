#include "hybrid_module_gates.hpp"

#include <fuse/ai/behavior_tree.hpp>
#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/hybrid_timeline_drive.hpp>
#include <fuse/cinematics/sprite_track.hpp>
#include <fuse/fx/afx_template_pack.hpp>
#include <fuse/fx/fx_socket.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>

#include <cmath>

namespace fuse::hybrid::gates {

namespace {

constexpr u32 kAgentObjectId = 7u;

void setupTimeline(fuse::cinematics::Timeline& timeline) {
    timeline.playhead().set_duration_ms(kTimelineDurationMs);
    fuse::cinematics::TrackGroup& group = timeline.add_group("HybridDirector");

    fuse::cinematics::SpriteTrack& spriteTrack = group.add_sprite_track("hud_sprite_drive");
    spriteTrack.set_target_sprite_id("hud_sprite");
    spriteTrack.add_keyframe({0, 0.f, 0.f, 1.f});
    spriteTrack.add_keyframe({kTimelineDurationMs, 40.f, 20.f, 1.f});
    spriteTrack.sort_keyframes();

    fuse::cinematics::CameraTrack& cameraTrack = group.add_camera_track("hybrid_camera");
    fuse::cinematics::CameraKeyframe start{};
    start.time_ms = 0;
    start.position = {0.f, 0.f, 5.f};
    start.field_of_view = 60.f;
    fuse::cinematics::CameraKeyframe end{};
    end.time_ms = kTimelineDurationMs;
    end.position = {0.f, 50.f, 10.f};
    end.field_of_view = 90.f;
    cameraTrack.add_keyframe(start);
    cameraTrack.add_keyframe(end);
    cameraTrack.sort_keyframes();

    fuse::cinematics::ActorTrack& actorTrack = group.add_actor_track("agent_mount");
    actorTrack.set_actor_id("agent_3d");
    fuse::cinematics::ActorEvent mount{};
    mount.time_ms = 500;
    mount.kind = fuse::cinematics::ActorEventKind::Mount;
    mount.actor_id = "agent_3d";
    mount.mount_point = "cockpit";
    actorTrack.add_actor_event(mount);
    actorTrack.sort_actor_events();

    timeline.play();
}

void setupFx(fuse::fx::FxComposer& fxComposer, const fuse::SceneObject3D& /*agent3D*/) {
    fxComposer.registerDemoVerticalSlice();
    fuse::fx::registerAfxTemplateSamplePack(fxComposer);

    fuse::fx::FxSocket spriteSocket;
    spriteSocket.kind = fuse::fx::FxSocketKind::Sprite2D;
    spriteSocket.effectId = "spark_burst";
    fxComposer.attach(spriteSocket);

    fuse::fx::FxSocket shapeSocket;
    shapeSocket.kind = fuse::fx::FxSocketKind::Shape3D;
    shapeSocket.effectId = "muzzle_flash";
    shapeSocket.owner = fuse::Handle<fuse::Object>(1u, 1u);
    fxComposer.attach(shapeSocket);
}

} // namespace

void setup(State& state, fuse::hybrid::HybridComposer& composer) {
    state.hudSprite.setPosition(0.f, 0.f);
    state.hudSprite.setLayer(10);
    state.world2D.addSprite(&state.hudSprite);

    state.agent3D.setPosition(-4.f, 0.f);
    state.agent3D.setZ(0.f);
    state.ally3D.setPosition(-3.5f, 0.5f);
    state.ally3D.setZ(0.f);
    state.lever3D.setPosition(3.f, 0.f);
    state.lever3D.setZ(0.f);
    state.world3D.addObject(&state.agent3D);
    state.world3D.addObject(&state.ally3D);
    state.world3D.addObject(&state.lever3D);

    state.initialClearR = 0.1f;
    state.initialClearG = 0.15f;
    state.initialClearB = 0.25f;
    state.world3D.setClearColor(state.initialClearR, state.initialClearG, state.initialClearB);

    const fuse::dimension::WorldHandle worldHandle(1u, 1u);
    state.world2D.loadWorld(worldHandle);
    state.world3D.loadWorld(worldHandle);

    composer.attachWorld2D(&state.world2D);
    composer.attachWorld3D(&state.world3D);

    state.aiRuntime.setTree(fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.12f));
    fuse::ai::AgentBinding agentBinding{};
    agentBinding.x = state.agent3D.x();
    agentBinding.y = state.agent3D.y();
    agentBinding.targetX = 4.f;
    agentBinding.targetY = 0.f;
    agentBinding.moveSpeed = 0.12f;
    agentBinding.teamId = 1;
    state.aiRuntime.addAgent(agentBinding);

    state.allyAiRuntime.setTree(fuse::ai::BehaviorTree::makePatrolWithAllySupportDemoTree(8.f));
    fuse::ai::AgentBinding allyBinding{};
    allyBinding.x = state.ally3D.x();
    allyBinding.y = state.ally3D.y();
    allyBinding.targetX = 4.f;
    allyBinding.targetY = 0.f;
    allyBinding.teamId = 1;
    state.allyAiRuntime.addAgent(allyBinding);

    fuse::ai::AgentBinding squadLeadBinding{};
    squadLeadBinding.x = state.agent3D.x();
    squadLeadBinding.y = state.agent3D.y();
    squadLeadBinding.teamId = 1;
    state.allyAiRuntime.addAgent(squadLeadBinding);

    state.vactorBridge.bind("agent_3d", &state.agent3D);

    setupTimeline(state.timeline);
    setupFx(state.fxComposer, state.agent3D);

    state.leverInteractable.setSupportedVerbs({"use"});
    state.leverInteractable.attach();
    state.mechanicsRegistry.registerInteractable(&state.leverInteractable, &state.leverInteractable);

    const fuse::mechanics::ConvexPolyhedron triggerVolume =
        fuse::mechanics::ConvexPolyhedron::axis_aligned_box(1.5f, -1.f, -1.f, 4.5f, 1.f, 1.f);
    state.leverTrigger.setPolyhedron(triggerVolume);
    state.leverConsole.registerMethod("toggleLever", [&state]() { state.leverToggle.toggle(); });
    state.leverTrigger.setOnEnter([&state](u32 /*objectId*/) {
        state.agentInsideTrigger = true;
        state.leverConsole.invoke("toggleLever");

        fuse::mechanics::InteractionContext mechanicsCtx;
        mechanicsCtx.verb = "use";
        state.mechanicsRegistry.interact(&state.leverInteractable, mechanicsCtx);

        fuse::adventure::InteractContext adventureCtx;
        adventureCtx.actorName = "player";
        state.hudPromptText = state.adventureSystem.showHudPrompt(adventureCtx, state.hudPrompt);
    });
}

void tickFrame(State& state, fuse::hybrid::HybridComposer& composer, const fuse::frame::FrameCtx& ctx) {
    composer.tick(const_cast<fuse::frame::FrameCtx&>(ctx));

    const fuse::cinematics::TimelineMs deltaMs =
        static_cast<fuse::cinematics::TimelineMs>(std::lround(ctx.dt * 1000.f));
    const fuse::cinematics::TimelineMs timelineBefore = state.timeline.playhead().time_ms();
    state.timeline.advance(deltaMs);
    fuse::cinematics::drain_actor_cues(state.timeline, state.vactorBridge, timelineBefore);
    state.lastTimelineMs = state.timeline.playhead().time_ms();

    const fuse::cinematics::HybridTimelineSample drive =
        fuse::cinematics::sample_hybrid_timeline_drive(state.timeline);
    state.hudSprite.setPosition(drive.spriteX, drive.spriteY);
    state.world3D.setClearColor(drive.clearR, drive.clearG, drive.clearB);

    state.aiRuntime.buildSnapshots();
    state.aiRuntime.evaluate(ctx);
    state.aiRuntime.commit();

    if (state.allyAiRuntime.agentCount() >= 2u) {
        state.allyAiRuntime.setBindingPosition(1u, state.agent3D.x(), state.agent3D.y());
    }
    state.allyAiRuntime.buildSnapshots();
    state.allyAiRuntime.evaluate(ctx);
    state.allyAiRuntime.commit();

    if (!state.aiRuntime.bindings().empty()) {
        const fuse::ai::AgentBinding& binding = state.aiRuntime.bindings()[0];
        state.agent3D.setPosition(binding.x, binding.y);
    }

    state.fxComposer.tick(ctx);

    state.leverTrigger.testObject(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
}

VerifyResult verify(const State& state, const fuse::hybrid::HybridComposer& composer) {
    if (composer.frameCount() != static_cast<fuse::u32>(kFrameCount)) {
        return {false, "all frames ticked"};
    }
    if (state.aiRuntime.tickCount() != static_cast<fuse::u32>(kFrameCount)) {
        return {false, "fuse_ai module ticked each frame"};
    }
    if (state.aiRuntime.bindings().empty() || state.aiRuntime.bindings()[0].x <= -3.5f) {
        return {false, "fuse_ai 3D agent committed movement toward target"};
    }
    if (state.agent3D.x() <= -3.5f) {
        return {false, "3D agent scene object moved by AI commit"};
    }
    if (state.hudSprite.x() < 10.f) {
        return {false, "fuse_cinematics sprite track drove HUD sprite"};
    }
    if (state.world3D.clearColorG() <= state.initialClearG + 0.01f) {
        return {false, "fuse_cinematics camera track drove 3D clear tint"};
    }
    if (state.fxComposer.attachmentCount() < 2u || state.fxComposer.tickCount() != static_cast<fuse::u32>(kFrameCount)) {
        return {false, "fuse_fx sockets ticked each frame"};
    }
    if (!state.allyAiRuntime.blackboard().flag(0, 1)) {
        return {false, "fuse_ai ally spatial squad flag set for hybrid ally agent"};
    }
    if (state.vactorBridge.mountCount() == 0u) {
        return {false, "fuse_cinematics VActor bridge applied mount cue"};
    }
    if (state.fxComposer.particlePool().spawnCount() == 0u) {
        return {false, "fuse_fx particle pool received spawns from active sockets"};
    }
    if (state.fxComposer.findEffect("afx_demo_spark") == nullptr) {
        return {false, "fuse_fx AFX-Template sample pack registered"};
    }
    if (state.leverToggle.toggleCount() == 0u) {
        return {false, "fuse_mechanics ToggleComponent toggled on polyhedron trigger enter"};
    }
    if (state.leverInteractable.interactionCount() == 0u) {
        return {false, "fuse_mechanics 3D interactable fired on trigger enter"};
    }
    if (state.hudPromptText != "Press E to activate lever") {
        return {false, "fuse_adventure HudPromptInteractable drove 2D HUD text"};
    }
    if (state.hudPrompt.promptShownCount() == 0u) {
        return {false, "fuse_adventure HUD prompt shown on examine"};
    }
    if (state.world2D.readSnapshot().sprites().size() != 1u) {
        return {false, "2D snapshot built via hierarchy fillSnapshotSoA"};
    }
    if (state.world3D.readSnapshot().objects().size() < 3u) {
        return {false, "3D snapshot includes agent + ally + lever objects"};
    }
    if (composer.renderer().sample(160, 120) == 0) {
        return {false, "3D clear colour present"};
    }
    if (composer.renderer().sample(172, 132) == 0) {
        return {false, "spinning 2D sprite visible"};
    }
    return {true, nullptr};
}

} // namespace fuse::hybrid::gates
