#include "hybrid_module_gates.hpp"

#include <fuse/ai/behavior_tree.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/hybrid_timeline_drive.hpp>
#include <fuse/cinematics/sprite_track.hpp>
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

    timeline.play();
}

void setupFx(fuse::fx::FxComposer& fxComposer, const fuse::SceneObject3D& /*agent3D*/) {
    fxComposer.registerDemoVerticalSlice();

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
    state.lever3D.setPosition(3.f, 0.f);
    state.lever3D.setZ(0.f);
    state.world3D.addObject(&state.agent3D);
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
    state.aiRuntime.addAgent(agentBinding);

    setupTimeline(state.timeline);
    setupFx(state.fxComposer, state.agent3D);

    state.leverInteractable.setSupportedVerbs({"use"});
    state.leverInteractable.attach();
    state.mechanicsRegistry.registerInteractable(&state.leverInteractable, &state.leverInteractable);

    fuse::mechanics::AxisAlignedBox bounds{};
    bounds.minX = 1.5f;
    bounds.maxX = 4.5f;
    bounds.minY = -1.f;
    bounds.maxY = 1.f;
    bounds.minZ = -1.f;
    bounds.maxZ = 1.f;
    state.leverTrigger.setBounds(bounds);
    state.leverTrigger.setOnEnter([&state](u32 /*objectId*/) {
        state.agentInsideTrigger = true;

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
    state.timeline.advance(deltaMs);

    const fuse::cinematics::HybridTimelineSample drive =
        fuse::cinematics::sample_hybrid_timeline_drive(state.timeline);
    state.hudSprite.setPosition(drive.spriteX, drive.spriteY);
    state.world3D.setClearColor(drive.clearR, drive.clearG, drive.clearB);

    state.aiRuntime.buildSnapshots();
    state.aiRuntime.evaluate(ctx);
    state.aiRuntime.commit();

    if (!state.aiRuntime.bindings().empty()) {
        const fuse::ai::AgentBinding& binding = state.aiRuntime.bindings()[0];
        state.agent3D.setPosition(binding.x, binding.y);
    }

    state.fxComposer.tick(ctx);

    state.leverTrigger.testObject(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.leverTrigger.advance(deltaMs);
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
    if (state.world3D.readSnapshot().objects().size() < 2u) {
        return {false, "3D snapshot includes agent + lever objects"};
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
