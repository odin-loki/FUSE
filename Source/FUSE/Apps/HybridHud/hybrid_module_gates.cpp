#include "hybrid_module_gates.hpp"

#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/uaisk_script_import.hpp>
#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/hybrid_timeline_drive.hpp>
#include <fuse/cinematics/sprite_track.hpp>
#include <fuse/cinematics/vactor_bridge.hpp>
#include <fuse/fx/afx_mission_hooks.hpp>
#include <fuse/fx/afx_template_pack.hpp>
#include <fuse/fx/fx_socket.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>

#include <cmath>

namespace fuse::hybrid::gates {

namespace {

constexpr u32 kAgentObjectId = 7u;
constexpr fuse::cinematics::TimelineMs kAssetTimeScale = 30;

void setupTimelineFromAsset(fuse::cinematics::Timeline& timeline) {
    std::string error;
    if (!fuse::cinematics::load_outpost_intro_30s_from_asset(timeline, &error)) {
        timeline = fuse::cinematics::make_outpost_intro_30s_stub();
    }

    timeline.playhead().set_duration_ms(30'000);
    timeline.play();
}

void setupFx(State& state) {
    state.fxComposer.registerDemoVerticalSlice();
    fuse::fx::registerAfxTemplateSamplePack(state.fxComposer);
    fuse::fx::registerAfxTemplateMissionHooks(state.fxComposer);
    fuse::fx::registerAfxTemplateMissionVm(state.fxComposer, state.missionScriptVm);
    state.missionScriptVm.dispatch("on_ambient_fx", state.fxComposer);

    fuse::fx::FxSocket spriteSocket;
    spriteSocket.kind = fuse::fx::FxSocketKind::Sprite2D;
    spriteSocket.effectId = "spark_burst";
    state.fxComposer.attach(spriteSocket);

    fuse::fx::FxSocket shapeSocket;
    shapeSocket.kind = fuse::fx::FxSocketKind::Shape3D;
    shapeSocket.effectId = "muzzle_flash";
    shapeSocket.owner = fuse::Handle<fuse::Object>(1u, 1u);
    state.fxComposer.attach(shapeSocket);
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
    state.guard3D.setPosition(2.f, 1.f);
    state.guard3D.setZ(0.f);
    state.lever3D.setPosition(3.f, 0.f);
    state.lever3D.setZ(0.f);
    state.world3D.addObject(&state.agent3D);
    state.world3D.addObject(&state.ally3D);
    state.world3D.addObject(&state.guard3D);
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

    state.aiRuntime.registerTreeProfile(0, fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.12f));
    fuse::ai::uaisk::registerPatrolSquadProfile(state.aiRuntime);
#if FUSE_HYBRID_GATES_SCRIPT
    state.scriptHost.init();
    state.scriptHostBridge.attach();
    state.scriptHostBridge.loadPatrolSquadViaHost();
#endif

    fuse::ai::AgentBinding agentBinding{};
    agentBinding.x = state.agent3D.x();
    agentBinding.y = state.agent3D.y();
    agentBinding.targetX = 4.f;
    agentBinding.targetY = 0.f;
    agentBinding.moveSpeed = 0.12f;
    agentBinding.teamId = 1;
    agentBinding.treeProfileId = 0;
    agentBinding.agent = fuse::Handle<fuse::Object>(1u, 1u);
    state.aiRuntime.addAgent(agentBinding);

    fuse::ai::AgentBinding allyBinding{};
    allyBinding.x = state.ally3D.x();
    allyBinding.y = state.ally3D.y();
    allyBinding.targetX = 4.f;
    allyBinding.targetY = 0.f;
    allyBinding.teamId = 1;
    allyBinding.treeProfileId = 1;
    state.aiRuntime.addAgent(allyBinding);

    fuse::ai::AgentBinding squadLeadBinding{};
    squadLeadBinding.x = state.agent3D.x();
    squadLeadBinding.y = state.agent3D.y();
    squadLeadBinding.teamId = 1;
    squadLeadBinding.treeProfileId = 1;
    state.aiRuntime.addAgent(squadLeadBinding);

    state.aiRuntime.setAgentPositionProvider([&state](fuse::Handle<fuse::Object> entity, float& outX, float& outY) {
        if (entity.index() == 1u) {
            outX = state.agent3D.x();
            outY = state.agent3D.y();
            return true;
        }
        return false;
    });

    state.vactorBridge.bind("agent_3d", &state.agent3D);
    state.vactorBridge.apply_shapebase_mount_chain("agent_3d", {"vehicle_seat", "turret"}, 10.f);
    state.vactorBridge.apply_shapebase_bone_attach("agent_3d", "weapon_shoulder");
    state.leverRadio.startBroadcast();
    state.weaponCombatLoop.setActiveWeapon(&state.weaponRuntime);
    state.weaponCombatLoop.setTarget(&state.guardHealth);

    setupTimelineFromAsset(state.timeline);
    setupFx(state);

    state.loadedOutpostStub = fuse::adventure::loadEmbeddedOutpostStub(state.outpostContent);
    state.spawnedOutpostInteractables =
        state.loadedOutpostStub &&
        fuse::adventure::spawnOutpostInteractables(state.outpostContent, state.outpostSpawn);
    state.appliedOutpostPlacements =
        state.spawnedOutpostInteractables &&
        fuse::adventure::applyOutpostScenePlacements(state.outpostSpawn, state.guard3D, state.lever3D);
    fuse::adventure::registerOutpostConversationScriptHooks(state.conversationScriptVm);

    state.leverInteractable.setSupportedVerbs({"use"});
    state.leverInteractable.attach();
    state.mechanicsRegistry.registerInteractable(&state.leverInteractable, &state.leverInteractable);

    const fuse::mechanics::ConvexPolyhedron triggerVolume =
        fuse::mechanics::ConvexPolyhedron::axis_aligned_box(-5.f, -1.f, -1.f, 4.5f, 1.f, 2.f);
    state.leverTrigger.setPolyhedron(triggerVolume);
    state.leverConsole.registerMethod("toggleLever", [&state]() {
        state.leverToggle.toggle();
        state.leverSwitch.flip();
    });
    state.leverTrigger.setOnEnter([&state](u32 /*objectId*/) {
        state.agentInsideTrigger = true;
        state.leverConsole.invoke("toggleLever");
        state.leverDelay.advance(100);
        state.leverRotate.advance(1.f / 60.f);
        state.leverCounter.increment();
        state.leverMessage.send();

        fuse::mechanics::InteractionContext mechanicsCtx;
        mechanicsCtx.verb = "use";
        state.mechanicsRegistry.interact(&state.leverInteractable, mechanicsCtx);

        fuse::adventure::InteractContext adventureCtx;
        adventureCtx.actorName = "player";
        state.hudPromptText = state.adventureSystem.showHudPrompt(adventureCtx, state.hudPrompt);

        auto weaponIt = state.outpostSpawn.weaponPickups.find("armory_rifle");
        if (!state.weaponGranted && weaponIt != state.outpostSpawn.weaponPickups.end() &&
            weaponIt->second != nullptr) {
            fuse::adventure::InteractContext grantCtx;
            grantCtx.inventory = &state.playerInventory;
            grantCtx.actorName = "player";
            fuse::adventure::WeaponGrantRequest grantRequest{};
            grantRequest.weapon = fuse::adventure::ItemId("plasma_rifle");
            grantRequest.ammo = fuse::adventure::ItemId("energy_cell");
            grantRequest.ammoAmount = 20;
            grantRequest.stats.damage = 25.f;
            grantRequest.stats.range = 80.f;
            fuse::adventure::SkeletalBoneMount boneMount{};
            boneMount.boneName = "weapon_shoulder";
            boneMount.yawDeg = 15.f;
            state.weaponSkeletalMount.setBoneMount(boneMount);
            state.weaponGranted = state.weaponGrantPipeline.grantOnPickupWithMount(
                grantCtx, *weaponIt->second, grantRequest, state.weaponRuntime, state.weaponMountAnim,
                state.weaponSkeletalMount);
            state.bindPoseBridge.applyMountToBindPose(state.weaponSkeletalMount, state.weaponMountAnim,
                                                      state.bindSkeleton, state.bindPose);
            state.weaponFired = state.weaponRuntime.fire(state.playerInventory);
            state.weaponCombatLoop.tick(1.f / 60.f);
            state.combatLoopFired = state.weaponCombatLoop.tryFire(state.playerInventory);
        }

        auto guardIt = state.outpostSpawn.conversations.find("outpost_guard");
        if (guardIt != state.outpostSpawn.conversations.end() && guardIt->second != nullptr) {
            fuse::adventure::InteractContext branchCtx;
            branchCtx.actorName = "player";
            branchCtx.inventory = &state.playerInventory;
            if (state.weaponGranted) {
                state.conversationScriptVm.dispatchBestBranch("outpost_guard", branchCtx, *guardIt->second);
                state.guardLineText = state.conversationScriptVm.lastLineDispatched();
                if (state.guardLineText.empty()) {
                    state.guardLineText =
                        state.adventureSystem.converseBranch(branchCtx, *guardIt->second,
                                                             state.conversationScriptVm.lastBranchDispatched());
                }
            } else {
                state.conversationScriptVm.dispatchBranch("outpost_guard", "polite", branchCtx, *guardIt->second);
                state.guardLineText =
                    state.adventureSystem.converseBranch(branchCtx, *guardIt->second, "polite");
            }
        }
    });

    state.physicsTriggerBridge.bind(&state.leverTrigger);
    state.physicsTriggerBridge.setPositionProvider([&state](fuse::u32 objectId) -> fuse::mechanics::PhysicsBodyPosition {
        if (objectId == kAgentObjectId) {
            return {state.agent3D.x(), state.agent3D.y(), state.agent3D.z()};
        }
        return {};
    });

    state.physicsBroadphaseBridge.bindTrigger(&state.leverTrigger);
    state.physicsBroadphaseBridge.setPositionProvider([&state](fuse::u32 objectId) -> fuse::mechanics::PhysicsBodyPosition {
        if (objectId == kAgentObjectId) {
            return {state.agent3D.x(), state.agent3D.y(), state.agent3D.z()};
        }
        return {};
    });
    state.physicsBroadphaseBridge.trackBody(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());

    state.patrolPath.addWaypoint(-4.f, 0.f, 0.f);
    state.patrolPath.addWaypoint(4.f, 0.f, 0.f);
    state.patrolPath.setPosition(state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.patrolPath.setLoop(true);
    state.patrolWaypoint.setPosition(4.f, 0.f, 0.f);
    state.guardLookAt.setTarget(state.agent3D.x(), state.agent3D.y());
    state.patrolTimer.setActive(true);
    state.patrolTimer.setOnFire([&state]() { state.leverAnimate.advance(0.25f); });
    state.bindSkeleton.bone_count = 16;
    state.bindSkeleton.bones.resize(16);
    state.bindPose = fuse::animation::PoseSoA::from_bind_pose(state.bindSkeleton);
    state.playerInventory.setMaxLimit(fuse::adventure::ItemId("plasma_rifle"), 1);
    state.playerInventory.setMaxLimit(fuse::adventure::ItemId("energy_cell"), 99);

    fuse::mechanics::BroadphaseWorldBody agentBody{};
    agentBody.objectId = kAgentObjectId;
    agentBody.proxy = fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
    agentBody.x = state.agent3D.x();
    agentBody.y = state.agent3D.y();
    agentBody.z = state.agent3D.z();
    state.broadphaseWorld.addBody(agentBody);

    state.broadphaseTriggerSync.bindTrigger(&state.leverTrigger);
    state.broadphaseTriggerSync.setPositionProvider([&state](fuse::u32 objectId) -> fuse::mechanics::PhysicsBodyPosition {
        if (objectId == kAgentObjectId) {
            return {state.agent3D.x(), state.agent3D.y(), state.agent3D.z()};
        }
        return {};
    });
    state.broadphaseTriggerSync.trackBody(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());

    const auto cuePreview = fuse::cinematics::preview_cues_at(state.timeline, 2'500);
    state.cuePreviewCount = static_cast<fuse::u32>(cuePreview.size());
}

void tickFrame(State& state, fuse::hybrid::HybridComposer& composer, const fuse::frame::FrameCtx& ctx) {
    composer.tick(const_cast<fuse::frame::FrameCtx&>(ctx));

    const fuse::cinematics::TimelineMs deltaMs =
        static_cast<fuse::cinematics::TimelineMs>(std::lround(ctx.dt * 1000.f * kAssetTimeScale));
    const fuse::cinematics::TimelineMs timelineBefore = state.timeline.playhead().time_ms();
    state.timeline.advance(deltaMs);
    fuse::cinematics::drain_actor_cues(state.timeline, state.vactorBridge, timelineBefore);
    state.vactorBridge.sync_bound_objects();
    state.vactorBridge.sync_motion_from_timeline(state.timeline);
    state.vactorBridge.sync_bone_attach_from_timeline(state.timeline);
    state.lastTimelineMs = state.timeline.playhead().time_ms();

    const fuse::cinematics::HybridTimelineSample drive =
        fuse::cinematics::sample_hybrid_timeline_drive(state.timeline);
    state.hudSprite.setPosition(drive.spriteX, drive.spriteY);
    state.world3D.setClearColor(drive.clearR, drive.clearG, drive.clearB);

    if (state.aiRuntime.agentCount() >= 3u) {
        state.aiRuntime.setBindingPosition(2u, state.agent3D.x(), state.agent3D.y());
    }
    state.aiRuntime.buildSnapshots();
    state.aiRuntime.evaluate(ctx);
    state.aiRuntime.commit();

    if (!state.aiRuntime.bindings().empty()) {
        const fuse::ai::AgentBinding& binding = state.aiRuntime.bindings()[0];
        state.agent3D.setPosition(binding.x, binding.y);
    }

    state.fxComposer.tick(ctx);
    state.missionScriptVm.dispatchTick(state.fxComposer, ctx);

    state.physicsBroadphaseBridge.trackBody(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.physicsBroadphaseBridge.syncBody(kAgentObjectId);
    state.broadphaseWorld.setBodyPosition(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.patrolPath.advanceAlongPath(20.f, ctx.dt);
    state.patrolTimer.tick(ctx.dt);
    state.guardLookAt.advanceTowardTarget(state.guard3D.x(), state.guard3D.y(), ctx.dt);
    if (state.patrolPath.x() >= 3.5f) {
        state.patrolWaypoint.markVisited();
    }
    state.broadphaseDbvtHits = state.broadphaseWorld.queryDbvtOverlaps(
        state.agent3D.x() - 2.f, state.agent3D.y() - 2.f, state.agent3D.z() - 2.f, state.agent3D.x() + 2.f,
        state.agent3D.y() + 2.f, state.agent3D.z() + 2.f);
    state.physicsTriggerBridge.syncObject(kAgentObjectId);
    state.broadphaseTriggerSync.trackBody(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.broadphaseTriggerSync.syncAll();
    state.leverTrigger.testObject(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.broadphaseRaycastHits = state.broadphaseWorld.queryRaycastStubFiltered(
        state.agent3D.x(), state.agent3D.y(), state.agent3D.z(), 1.f, 0.f, 0.f, 8.f,
        fuse::mechanics::BroadphaseProxyFilter::Character);
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
    if (state.aiRuntime.treeProfileCount() < 2u) {
        return {false, "fuse_ai per-agent tree profiles registered"};
    }
    if (!state.aiRuntime.blackboard().flag(1, 1)) {
        return {false, "fuse_ai ally spatial squad flag set for hybrid ally agent"};
    }
#if FUSE_HYBRID_GATES_SCRIPT
    if (state.scriptHostBridge.importCount() == 0u) {
        return {false, "fuse_ai ScriptHost UAISK import progressed"};
    }
#endif
    if (state.hudSprite.x() < 5.f) {
        return {false, "fuse_cinematics sprite track drove HUD sprite from 30s asset"};
    }
    if (state.world3D.clearColorG() <= state.initialClearG + 0.01f) {
        return {false, "fuse_cinematics camera track drove 3D clear tint"};
    }
    if (state.vactorBridge.mountCount() == 0u) {
        return {false, "fuse_cinematics VActor bridge applied mount cue"};
    }
    if (state.vactorBridge.shapebaseAttachCount() == 0u) {
        return {false, "fuse_cinematics ShapeBase VActor attach applied"};
    }
    if (state.vactorBridge.syncCount() == 0u) {
        return {false, "fuse_cinematics ShapeBase attach sync applied"};
    }
    if (state.vactorBridge.motionSyncCount() == 0u) {
        return {false, "fuse_cinematics ShapeBase motion sync applied"};
    }
    if (state.cuePreviewCount == 0u) {
        return {false, "fuse_cinematics cue preview stub collected cues"};
    }
    if (state.fxComposer.attachmentCount() < 2u || state.fxComposer.tickCount() != static_cast<fuse::u32>(kFrameCount)) {
        return {false, "fuse_fx sockets ticked each frame"};
    }
    if (state.fxComposer.particlePool().spawnCount() == 0u) {
        return {false, "fuse_fx particle pool received spawns from active sockets"};
    }
    if (state.fxComposer.particlePoolGpu().syncCount() == 0u) {
        return {false, "fuse_fx GPU particle pool backend synced"};
    }
    if (state.fxComposer.particlePoolGpu().cudaSkipCount() == 0u) {
        return {false, "fuse_fx CUDA particle pool skip-clean path exercised"};
    }
    if (state.fxComposer.particlePoolGpu().lastCudaSkipReason() ==
        fuse::fx::ParticlePoolCudaSkipReason::None) {
        return {false, "fuse_fx CUDA skip reason recorded"};
    }
    if (state.fxComposer.findEffect("afx_demo_spark") == nullptr) {
        return {false, "fuse_fx AFX-Template sample pack registered"};
    }
    if (state.missionScriptVm.dispatchCount() == 0u) {
        return {false, "fuse_fx AFX-Template mission script VM dispatched"};
    }
    if (state.leverToggle.toggleCount() == 0u) {
        return {false, "fuse_mechanics ToggleComponent toggled on polyhedron trigger enter"};
    }
    if (state.leverDelay.fireCount() == 0u) {
        return {false, "fuse_mechanics DelayComponent fired on trigger enter"};
    }
    if (state.leverRotate.tickCount() == 0u) {
        return {false, "fuse_mechanics RotateComponent advanced on trigger enter"};
    }
    if (state.leverCounter.incrementCount() == 0u) {
        return {false, "fuse_mechanics CounterComponent incremented on trigger enter"};
    }
    if (state.leverMessage.sendCount() == 0u) {
        return {false, "fuse_mechanics MessageComponent sent on trigger enter"};
    }
    if (state.leverSwitch.switchCount() == 0u) {
        return {false, "fuse_mechanics SwitchComponent flipped on trigger enter"};
    }
    if (state.physicsTriggerBridge.syncCount() == 0u) {
        return {false, "fuse_mechanics physics trigger polyhedron bridge synced"};
    }
    if (state.broadphaseTriggerSync.syncCount() == 0u) {
        return {false, "fuse_mechanics broadphase trigger sync progressed"};
    }
    if (state.physicsBroadphaseBridge.syncCount() == 0u) {
        return {false, "fuse_mechanics physics broadphase pipeline synced"};
    }
#if FUSE_HYBRID_GATES_WAVE16
    if (state.patrolPath.tickCount() == 0u) {
        return {false, "fuse_mechanics PathComponent advanced in hybrid demo"};
    }
    if (state.patrolTimer.fireCount() == 0u) {
        return {false, "fuse_mechanics TimerComponent fired in hybrid demo"};
    }
    if (state.broadphaseWorld.bodyCount() == 0u) {
        return {false, "fuse_mechanics BroadphaseWorldStub tracked agent body"};
    }
    if (state.vactorBridge.boneMotionSyncCount() == 0u) {
        return {false, "fuse_cinematics bone attach motion sync applied"};
    }
#endif
#if FUSE_HYBRID_GATES_WAVE19
    if (state.vactorBridge.mountChainDepth() < 2u) {
        return {false, "fuse_cinematics ShapeBase mount chain depth applied"};
    }
    if (!state.leverRadio.broadcasting() || state.leverRadio.broadcastCount() == 0u) {
        return {false, "fuse_mechanics RadioComponent broadcast in hybrid demo"};
    }
    if (!state.combatLoopFired || state.weaponCombatLoop.damageApplyCount() == 0u) {
        return {false, "fuse_adventure WeaponCombatLoop applied damage after grant"};
    }
    if (state.guardHealth.damageEvents() == 0u) {
        return {false, "fuse_adventure combat loop damaged guard health"};
    }
#endif
#if FUSE_HYBRID_GATES_WAVE18
    if (state.vactorBridge.mountRotationSyncCount() == 0u) {
        return {false, "fuse_cinematics ShapeBase mount rotation sync applied"};
    }
    if (state.fxComposer.particlePoolGpu().residencySyncCount() == 0u) {
        return {false, "fuse_fx CUDA particle residency sync in composer tick"};
    }
    if (state.patrolWaypoint.visitCount() == 0u) {
        return {false, "fuse_mechanics WaypointComponent visited in hybrid demo"};
    }
    if (state.guardLookAt.tickCount() == 0u) {
        return {false, "fuse_mechanics LookAtComponent tracked agent in hybrid demo"};
    }
    if (state.broadphaseDbvtHits == 0u) {
        return {false, "fuse_mechanics BroadphaseWorldStub dbvt query hit agent"};
    }
    if (!state.weaponFired || state.weaponRuntime.fireCount() == 0u) {
        return {false, "fuse_adventure weapon runtime fired after grant in hybrid demo"};
    }
#endif
#if FUSE_HYBRID_GATES_WAVE17
    if (state.fxComposer.particlePoolGpu().selectiveWritebackCount() == 0u) {
        return {false, "fuse_fx selective CUDA writeback in composer tick"};
    }
    if (state.patrolPath.loopCount() == 0u) {
        return {false, "fuse_mechanics PathComponent looped in hybrid demo"};
    }
    if (state.patrolTimer.callbackFireCount() == 0u) {
        return {false, "fuse_mechanics TimerComponent onFire callback fired"};
    }
    if (state.leverAnimate.cycleCount() == 0u) {
        return {false, "fuse_mechanics AnimateComponent advanced via timer callback"};
    }
    if (state.broadphaseRaycastHits == 0u) {
        return {false, "fuse_mechanics BroadphaseWorldStub filtered raycast hit agent"};
    }
    if (!state.weaponGranted) {
        return {false, "fuse_adventure weapon grant pipeline granted armory rifle"};
    }
    if (state.weaponGrantPipeline.mountGrantCount() == 0u) {
        return {false, "fuse_adventure weapon mount grant chain applied"};
    }
    if (state.bindPoseBridge.applyCount() == 0u) {
        return {false, "fuse_adventure animation bind pose bridge applied on grant"};
    }
    if (state.guardLineText != "I see you are armed. Keep that rifle stowed.") {
        return {false, "fuse_adventure armed conversation branch via dispatchBestBranch"};
    }
    if (state.conversationScriptVm.lastBranchDispatched() != "armed") {
        return {false, "fuse_adventure conversation VM chose armed priority branch"};
    }
#endif
    if (state.leverInteractable.interactionCount() == 0u) {
        return {false, "fuse_mechanics 3D interactable fired on trigger enter"};
    }
    if (!state.loadedOutpostStub) {
        return {false, "fuse_adventure outpost_stub.json loaded"};
    }
    if (!state.spawnedOutpostInteractables) {
        return {false, "fuse_adventure door/weapon spawned from loaded JSON"};
    }
    if (!state.appliedOutpostPlacements) {
        return {false, "fuse_adventure scene placements applied from JSON transforms"};
    }
    if (state.guard3D.x() < 1.5f) {
        return {false, "fuse_adventure outpost_guard placed from JSON x/y/z"};
    }
    if (state.outpostSpawn.doors.find("maintenance_door") == state.outpostSpawn.doors.end()) {
        return {false, "fuse_adventure maintenance door spawned from JSON"};
    }
    if (state.outpostSpawn.weaponPickups.find("armory_rifle") == state.outpostSpawn.weaponPickups.end()) {
        return {false, "fuse_adventure armory rifle spawned from JSON"};
    }
    if (state.hudPromptText != "Press E to activate lever") {
        return {false, "fuse_adventure HudPromptInteractable drove 2D HUD text"};
    }
    if (state.hudPrompt.promptShownCount() == 0u) {
        return {false, "fuse_adventure HUD prompt shown on examine"};
    }
#if !FUSE_HYBRID_GATES_WAVE17
    if (state.guardLineText != "Thank you, traveler. Proceed with caution.") {
        return {false, "fuse_adventure NPC conversation branch in hybrid demo"};
    }
#endif
    if (state.conversationScriptVm.dispatchCount() == 0u) {
        return {false, "fuse_adventure conversation script VM dispatched branch"};
    }
    if (state.world2D.readSnapshot().sprites().size() != 1u) {
        return {false, "2D snapshot built via hierarchy fillSnapshotSoA"};
    }
    if (state.world3D.readSnapshot().objects().size() < 4u) {
        return {false, "3D snapshot includes agent + ally + guard + lever objects"};
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
