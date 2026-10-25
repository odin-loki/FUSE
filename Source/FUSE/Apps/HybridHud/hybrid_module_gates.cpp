#include "hybrid_module_gates.hpp"

#include <fuse/ai/behavior_tree.hpp>
#include <fuse/ai/uaisk_cs_codegen.hpp>
#include <fuse/ai/uaisk_cs_syntax_tree.hpp>
#include <fuse/ai/uaisk_script_import.hpp>
#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/audio_track.hpp>
#include <fuse/cinematics/event_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/hybrid_timeline_drive.hpp>
#include <fuse/cinematics/sprite_track.hpp>
#include <fuse/cinematics/vactor_bridge.hpp>
#include <fuse/fx/afx_mission_loader.hpp>
#include <fuse/fx/afx_mission_hooks.hpp>
#include <fuse/fx/afx_template_pack.hpp>
#include <fuse/fx/residual_effects.hpp>
#include <fuse/fx/fx_socket.hpp>
#include <fuse/fx/missile_descriptor.hpp>
#include <fuse/fx/spell_descriptor.hpp>
#include <fuse/fx/parameter_bind.hpp>
#include <fuse/fx/socket_constraint.hpp>
#include <fuse/ai/blackboard.hpp>
#include <fuse/ai/spatial_query.hpp>
#include <fuse/ai/uaisk_template_hooks.hpp>
#include <fuse/mechanics/broadphase_proxy_filter.hpp>
#include <fuse/cinematics/mount_orientation.hpp>
#include <fuse/mechanics/interact_action.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fuse::hybrid::gates {

namespace {

constexpr u32 kAgentObjectId = 7u;
constexpr u32 kLeaveSimObjectId = 8u;
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

    static const char* kScheduledMisText =
        "missionName = \"HybridHudSchedule\";\n"
        "schedule(50, onAmbientFx);\n"
        "function onAmbientFx() {\n"
        "  attachEffect(\"spark_burst\");\n"
        "}\n";
    std::string scheduleError;
    (void)fuse::fx::dispatch_afx_mission_from_mis(kScheduledMisText, state.fxComposer, state.missionScriptVm,
                                                 &scheduleError);
    state.missionScheduledPendingDelayed = state.missionScriptVm.pendingDelayedCount() > 0u;

    static const char* kExecuteMisText =
        "function onSpellCast() {\n"
        "  beginCast(\"fireball\");\n"
        "}\n"
        "function onAmbientFx() {\n"
        "  attachEffect(\"spark_burst\");\n"
        "}\n";
    std::string executeError;
    (void)fuse::fx::execute_afx_mission_from_mis(kExecuteMisText, state.fxComposer, state.missionScriptVm,
                                                &executeError);

    static const char* kSpellMisText =
        "new SimObject(FireballSpell) {\n"
        "  spellId = \"fireball\";\n"
        "}\n";
    fuse::fx::AfxMissionBody spellBody;
    if (fuse::fx::parse_afx_mission_body_from_mis(kSpellMisText, spellBody)) {
        std::vector<fuse::fx::AfxMissionBodySpell> spells;
        state.missionSpellCodegenCount = fuse::fx::codegen_spells_from_mission_body(spellBody, spells);
        if (!spells.empty()) {
            state.missionSpellCodegenId = spells[0].spellId;
        }
    }

    static const char* kNestedSimObjectMisText =
        "new SimObject(FireballGroup) {\n"
        "  new SimObject(SparkEmitter) { effectName = \"spark\"; }\n"
        "  new SimObject(MuzzleFlashEmitter) { effectName = \"muzzle\"; }\n"
        "}\n";
    fuse::fx::AfxMissionBody nestedBody;
    if (fuse::fx::parse_afx_mission_body_from_mis(kNestedSimObjectMisText, nestedBody)) {
        state.missionNestedSimObjectCount = static_cast<u32>(nestedBody.nestedSimObjectBodies.size());
        std::vector<fuse::fx::AfxMissionBodyEffect> missionCodegenEffects;
        state.missionEffectCodegenCount =
            fuse::fx::codegen_effects_from_mission_body(nestedBody, missionCodegenEffects);
    }

    static const char* kMisText =
        "function onSpellCast(%caster, %spell) {\n"
        "}\n"
        "function onAmbientFx() {\n"
        "}\n"
        "function onImpactFx() {\n"
        "}\n"
        "function onSpellReady() {\n"
        "}\n"
        "function onTick() {\n"
        "}\n"
        "%on_spell_cast = \"fireball\"\n";
    {
        std::vector<fuse::fx::AfxMissionHook> misHooksProbe;
        if (fuse::fx::load_afx_mission_hooks_from_mis(kMisText, misHooksProbe, nullptr)) {
            state.missionMisHookParseCount = static_cast<fuse::u32>(misHooksProbe.size());
        }
    }
    std::string misRegisterError;
    if (fuse::fx::register_afx_mission_from_mis(kMisText, state.fxComposer, state.missionScriptVm,
                                                &misRegisterError)) {
        state.missionRegisterHookCount = state.missionScriptVm.hookCount();
        const fuse::u32 impactDispatchBefore = state.missionScriptVm.dispatchCount();
        (void)state.missionScriptVm.dispatch("on_impact_fx", state.fxComposer);
        state.missionImpactHookDispatched =
            state.missionScriptVm.dispatchCount() > impactDispatchBefore &&
            state.missionScriptVm.lastHookDispatched() == "on_impact_fx";
        const fuse::u32 spellReadyBefore = state.missionScriptVm.dispatchCount();
        (void)state.missionScriptVm.dispatch("on_spell_ready", state.fxComposer);
        state.missionSpellReadyDispatched =
            state.missionScriptVm.dispatchCount() > spellReadyBefore &&
            state.missionScriptVm.lastHookDispatched() == "on_spell_ready";
        const fuse::u32 executeBefore = state.missionScriptVm.executeCount();
        (void)state.missionScriptVm.executeFunctionBody("onImpactFx", "trigger impact burst", state.fxComposer);
        state.missionExecuteImpactViaBody = state.missionScriptVm.executeCount() > executeBefore;
    }

    static const char* kSpellAssignMisText =
        "function onSpellCast(%caster, %spell) {\n"
        "}\n"
        "%on_spell_cast = \"fireball\"\n";
    std::vector<fuse::fx::AfxMissionHook> spellHooks;
    if (fuse::fx::load_afx_mission_hooks_from_mis(kSpellAssignMisText, spellHooks, nullptr)) {
        state.missionSpellAssignmentApplied =
            fuse::fx::apply_afx_mission_spell_assignments(kSpellAssignMisText, spellHooks);
        for (const fuse::fx::AfxMissionHook& hook : spellHooks) {
            if (hook.scriptHook == "on_spell_cast" && hook.spellId == "fireball") {
                state.missionSpellAssignmentApplied = true;
            }
        }
    }

    fuse::fx::FxSocket spriteSocket;
    spriteSocket.kind = fuse::fx::FxSocketKind::Sprite2D;
    spriteSocket.effectId = "spark_burst";
    state.fxComposer.attach(spriteSocket);

    fuse::fx::FxSocket shapeSocket;
    shapeSocket.kind = fuse::fx::FxSocketKind::Shape3D;
    shapeSocket.effectId = "muzzle_flash";
    shapeSocket.owner = fuse::Handle<fuse::Object>(1u, 1u);
    state.fxComposer.attach(shapeSocket);

    state.fxComposer.residuals().setSpawnHook([&state](const fuse::fx::ResidualEntry&) {
        ++state.residualSpawnCount;
    });

    fuse::fx::ChoreographerBinding choreoBinding;
    choreoBinding.socket.kind = fuse::fx::FxSocketKind::Sprite2D;
    choreoBinding.socket.effectId = "spark_burst";
    choreoBinding.spellId = "fireball";
    choreoBinding.beginCastOnAttach = true;
    (void)state.choreographerBridge.bindSocket(state.fxComposer, choreoBinding);

    fuse::fx::EffectGraph& graph = state.fxComposer.effectGraph();
    const fuse::u32 graphParent = graph.addNode("spark_burst");
    (void)graph.addNode("muzzle_flash", graphParent);
    graph.activate();

    fuse::fx::MissileDescriptor hybridMissile = fuse::fx::MissileDescriptor::makeFireballMissile();
    hybridMissile.lifetime = 0.5f;
    state.fxComposer.missiles().fire(hybridMissile, {state.agent3D.x(), state.agent3D.y(), state.agent3D.z()},
                                     {0.f, 0.f, 1.f});

    fuse::fx::bind::ParameterBinder castBinder;
    castBinder.bindHandle("caster", fuse::Handle<fuse::Object>(1u, 1u));
    castBinder.bindHandle("target", fuse::Handle<fuse::Object>(2u, 1u));
    castBinder.bindFloat("intensity", 1.25f);
    state.parameterBindSlotCount = castBinder.count();
    const fuse::fx::CastBinding boundCast = castBinder.resolveCastBinding();
    state.castBeginOk = state.fxComposer.beginCast("fireball", boundCast);

    fuse::fx::SocketConstraintManager socketConstraints;
    (void)socketConstraints.defineConstraint(fuse::fx::ConstraintKind::Point, "impact");
    (void)socketConstraints.setReferencePoint(
        "impact", {state.lever3D.x(), state.lever3D.y(), state.lever3D.z()});
    fuse::fx::FxSocket constrainedSocket;
    constrainedSocket.effectId = "spark_burst";
    state.socketConstraintRemapped = socketConstraints.remapSocket(constrainedSocket, "impact");
    fuse::fx::ConstraintDef constraintSpec;
    state.constraintSpecParsed = fuse::fx::parse_constraint_spec("point:impact", constraintSpec);
    fuse::fx::ConstraintDef shapeConstraintSpec;
    state.shapeConstraintParsed = fuse::fx::parse_constraint_spec("shape:muzzle", shapeConstraintSpec);
    (void)socketConstraints.sample("impact");
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

    // E03: the renderable 3D scene (ECS registry of the world): a floor slab, the lever as a gold cube, a sun, and the
    // camera. The GPU path renders it through the SceneRenderer; the placeholder fallback ignores it.
    {
        const float floorCenter[3] = {0.f, -0.1f, -3.f};
        const float floorHalf[3] = {5.f, 0.1f, 5.f};
        const float leverCenter[3] = {0.f, 0.4f, -3.f};
        const float leverHalf[3] = {0.4f, 0.4f, 0.4f};
        (void)state.world3D.spawnMesh(fuse::world3d::BuiltinMesh::Cube, 0u, floorCenter, floorHalf);
        (void)state.world3D.spawnMesh(fuse::world3d::BuiltinMesh::Cube, 4u, leverCenter, leverHalf);
        const float toSun[3] = {0.4f, 0.85f, 0.35f};
        const float sunColor[3] = {1.f, 0.96f, 0.9f};
        (void)state.world3D.spawnDirectionalLight(toSun, sunColor, 3.f);
        fuse::world3d::RenderCamera3D camera{};
        camera.eye[0] = 0.f;
        camera.eye[1] = 1.8f;
        camera.eye[2] = 1.5f;
        camera.target[0] = 0.f;
        camera.target[1] = 0.4f;
        camera.target[2] = -3.f;
        camera.valid = true;
        state.world3D.setCamera(camera);
    }

    composer.attachWorld2D(&state.world2D);
    composer.attachWorld3D(&state.world3D);

    state.aiRuntime.registerTreeProfile(0, fuse::ai::BehaviorTree::makeMoveTowardDemoTree(0.12f));
    fuse::ai::uaisk::registerPatrolSquadProfile(state.aiRuntime);
    static const char* kNestedCompositeCs =
        "class CompositePatrol : BehaviorBase {\n"
        "  composite = \"selector\";\n"
        "  behaviorTree = \"aiComposite.cs\";\n"
        "  void onNestedPatrol() {}\n"
        "}\n";
    fuse::ai::BehaviorTree nestedCompositeTree;
    std::string nestedCompositeError;
    if (fuse::ai::uaisk::codegenTreeFromSyntaxTree("aiComposite.cs", kNestedCompositeCs, nestedCompositeTree,
                                                    &nestedCompositeError)) {
        state.nestedCompositeNodeCount = nestedCompositeTree.nodeCount();
        state.aiRuntime.registerTreeProfile(2, std::move(nestedCompositeTree));
    }
    static const char* kTargetingPatrolCs =
        "class TargetingPatrol : BehaviorBase {\n"
        "  condition = \"distance < 12\";\n"
        "  distanceThreshold = 5.f;\n"
        "  behaviorTree = \"aiTargeting.cs\";\n"
        "}\n";
    fuse::ai::BehaviorTree expressionAstTree;
    std::string expressionAstError;
    if (fuse::ai::uaisk::codegenTreeFromSyntaxTree("aiTargeting.cs", kTargetingPatrolCs, expressionAstTree,
                                                    &expressionAstError)) {
        state.uaiskExpressionNodeCount = expressionAstTree.nodeCount();
        state.aiRuntime.registerTreeProfile(3, std::move(expressionAstTree));
    }
    fuse::ai::uaisk::UaiskCsSyntaxTree targetingSyntaxTree;
    fuse::ai::BehaviorTree codegenCsTree;
    if (fuse::ai::uaisk::codegenTreeFromCs("aiTargeting.cs", kTargetingPatrolCs, codegenCsTree, nullptr)) {
        state.codegenCsTreeNodeCount = codegenCsTree.nodeCount();
    }
    fuse::ai::uaisk::UaiskCsParseResult csParseResult;
    state.uaiskCsModuleParsed =
        fuse::ai::uaisk::parseCsModule("aiTargeting.cs", kTargetingPatrolCs, csParseResult);
    if (fuse::ai::uaisk::parseCsSyntaxTree("aiTargeting.cs", kTargetingPatrolCs, targetingSyntaxTree)) {
        fuse::ai::uaisk::UaiskCsAst targetingAst;
        if (fuse::ai::uaisk::buildAstFromSyntaxTree(targetingSyntaxTree, targetingAst) &&
            !targetingAst.conditions.empty()) {
            state.uaiskExpressionAstValid = targetingAst.conditions[0].valid;
            state.uaiskExpressionThreshold = targetingAst.conditions[0].threshold;
        }
    }
    {
        namespace fs = std::filesystem;
        const fs::path tempPath = fs::temp_directory_path() / "fuse_hybrid_patrol_fsevents.bt";
        {
            std::ofstream out(tempPath);
            out << "bb.action.set_flag flag=1\nroot=0\n";
        }
        state.treeFileWatchRegistry.watchProfileFromDisk(tempPath.string(), 8u);
        const fuse::u32 fseventsPollsBefore = state.treeFileWatchRegistry.fseventsPollCount();
        (void)state.treeFileWatchRegistry.pollFSEventsFileChanges(state.aiRuntime);
        {
            std::ofstream out(tempPath, std::ios::trunc);
            out << "bb.action.set_flag flag=0\nroot=0\n";
        }
        state.treeFileWatchFseventsReloadCount =
            state.treeFileWatchRegistry.pollFSEventsFileChanges(state.aiRuntime);
        state.treeFileWatchFseventsPollCount =
            state.treeFileWatchRegistry.fseventsPollCount() - fseventsPollsBefore;
        fs::remove(tempPath);
    }
    {
        namespace fs = std::filesystem;
        const fs::path osPath = fs::temp_directory_path() / "fuse_hybrid_patrol_os_poll.bt";
        {
            std::ofstream out(osPath);
            out << "bb.action.set_flag flag=2\nroot=0\n";
        }
        state.treeFileWatchRegistry.watchProfileFromDisk(osPath.string(), 9u);
        const fuse::u32 osPollsBefore = state.treeFileWatchRegistry.osPollCount();
        (void)state.treeFileWatchRegistry.pollOsFileChanges(state.aiRuntime);
        state.treeFileWatchOsPollCount = state.treeFileWatchRegistry.osPollCount() - osPollsBefore;
        fs::remove(osPath);
    }
    {
        namespace fs = std::filesystem;
        const fs::path inotifyPath = fs::temp_directory_path() / "fuse_hybrid_patrol_inotify.bt";
        {
            std::ofstream out(inotifyPath);
            out << "bb.action.set_flag flag=3\nroot=0\n";
        }
        state.treeFileWatchRegistry.watchProfileFromDisk(inotifyPath.string(), 10u);
        const fuse::u32 inotifyPollsBefore = state.treeFileWatchRegistry.inotifyPollCount();
        (void)state.treeFileWatchRegistry.pollInotifyFileChanges(state.aiRuntime);
        {
            std::ofstream out(inotifyPath, std::ios::trunc);
            out << "bb.action.set_flag flag=4\nroot=0\n";
        }
        state.treeInotifyReloadCount = state.treeFileWatchRegistry.pollInotifyFileChanges(state.aiRuntime);
        state.treeInotifyPollCount = state.treeFileWatchRegistry.inotifyPollCount() - inotifyPollsBefore;
        fs::remove(inotifyPath);
    }
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

    state.aiRuntime.buildSnapshots();
    {
        float scalarProbe = 0.f;
        state.blackboardScalarRoundTrip =
            state.aiRuntime.blackboard().trySetScalar(0, 2, 2.5f) &&
            state.aiRuntime.blackboard().tryGetScalar(0, 2, scalarProbe) && scalarProbe == 2.5f;
    }

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
    state.setupMountChainDepth = state.vactorBridge.mountChainDepth();
    state.combinedMountEuler = fuse::cinematics::combine_mount_euler_deg(
        fuse::cinematics::MountEulerDeg{10.f, 0.f, 0.f}, fuse::cinematics::MountEulerDeg{7.5f, 2.f, 0.f});
    state.combinedMountYawDeg = fuse::cinematics::combine_mount_yaw_deg(10.f, 7.5f);
    const std::vector<fuse::cinematics::MountEulerDeg> mountChainEuler = {
        {10.f, 0.f, 0.f},
        {7.5f, 2.f, 0.f},
    };
    const fuse::cinematics::MountEulerDeg chainEuler =
        fuse::cinematics::combine_mount_chain_euler(mountChainEuler);
    (void)chainEuler;
    const fuse::cinematics::MountQuaternion mountQuat = fuse::cinematics::yaw_deg_to_quaternion(10.f);
    const fuse::cinematics::MountQuaternion eventQuat = fuse::cinematics::yaw_deg_to_quaternion(7.5f);
    state.combinedMountQuatYawDeg = fuse::cinematics::quaternion_to_yaw_deg(
        fuse::cinematics::combine_mount_orientation(mountQuat, eventQuat));
    const fuse::cinematics::MountQuaternion eulerAsQuat =
        fuse::cinematics::euler_deg_to_quaternion(state.combinedMountEuler);
    state.mountEulerRoundTripYaw = fuse::cinematics::quaternion_to_euler_deg(eulerAsQuat).yaw_deg;
    const std::vector<fuse::cinematics::MountQuaternion> mountQuatChain = {mountQuat, eventQuat};
    (void)fuse::cinematics::combine_mount_chain(mountQuatChain);
    state.vactorBridge.apply_shapebase_bone_attach("agent_3d", "weapon_shoulder");
    state.leverRadio.startBroadcast();
    state.weaponCombatLoop.setActiveWeapon(&state.weaponRuntime);
    state.weaponCombatLoop.setTarget(&state.guardHealth);
    state.weaponCombatLoop.setHitscanStub(&state.combatHitscan);
    state.weaponCombatLoop.setTargetPosition(state.guard3D.x(), state.guard3D.y(), state.guard3D.z());
    state.combatHitscan.setSpreadDeg(state.weaponRuntime.stats().spreadDeg);
    {
        const float dx = state.guard3D.x() - state.agent3D.x();
        const float dy = state.guard3D.y() - state.agent3D.y();
        const float dz = state.guard3D.z() - state.agent3D.z();
        const float aimLen = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float dirX = aimLen > 0.001f ? dx / aimLen : 1.f;
        const float dirY = aimLen > 0.001f ? dy / aimLen : 0.f;
        const float dirZ = aimLen > 0.001f ? dz / aimLen : 0.f;
        const fuse::adventure::HitscanResult hitResult = state.combatHitscan.fireHitscan(
            state.weaponRuntime.stats(), state.agent3D.x(), state.agent3D.y(), state.agent3D.z(), dirX, dirY, dirZ,
            state.guard3D.x(), state.guard3D.y(), state.guard3D.z());
        state.directHitscanOk = hitResult.hit && hitResult.damage > 0.f;
    }

    setupTimelineFromAsset(state.timeline);
    static const char* kTimelineHostAssetText =
        "# Outpost intro 30s sequence\n"
        "duration_ms=30000\n"
        "sprite hud_sprite 0,-20,0,1 15000,0,10,1 30000,40,20,1\n"
        "camera 0,0,0,8,55 15000,0,30,12,70 30000,0,60,15,85\n"
        "actor agent_3d mount 2000 cockpit 15\n";
    state.timelineHostWired =
        state.timelineHostStub.wireFromTimeline(state.timeline, kTimelineHostAssetText, 2'500);
    {
        fuse::cinematics::TrackGroup& scrubGroup = state.timeline.add_group("OutpostScriptCues");
        fuse::cinematics::EventTrack& scrubEvents = scrubGroup.add_event_track("ScriptCues");
        scrubEvents.set_script_hook_id("on_hybrid_scrub");
        state.eventTrackScriptHookOk = scrubEvents.script_hook_id() == "on_hybrid_scrub";
        scrubEvents.add_event(fuse::cinematics::TimelineEvent("door_open", 1'000));
        scrubEvents.add_event(fuse::cinematics::TimelineEvent("dialog_start", 3'000));
        scrubEvents.sort_events();
        fuse::cinematics::AudioTrack& reactorAlarm = scrubGroup.add_audio_track("ReactorHum");
        reactorAlarm.set_sound_asset_id("Audio/reactor_hum");
        reactorAlarm.add_keyframe({0, 0.2f});
        reactorAlarm.add_keyframe({5'000, 1.0f});
        state.reactorAlarmVolumeSample = reactorAlarm.volume_at(2'500);
        state.leverOpenPropertyTrack = &scrubGroup.add_property_track("LeverOpen");
        state.leverOpenPropertyTrack->set_property_path("lever.open");
        state.leverOpenPropertyTrack->add_keyframe({0, 0.f});
        state.leverOpenPropertyTrack->add_keyframe({5'000, 1.f});
        state.leverOpenPropertyMidRampSample = state.leverOpenPropertyTrack->sample_at(2'500);
        state.leverOpenPropertySample = state.leverOpenPropertyMidRampSample;
        fuse::cinematics::MotionTrack& patrolMotion = scrubGroup.add_motion_track("AgentPatrol");
        patrolMotion.set_target_object_id("agent_3d");
        patrolMotion.path().add_waypoint({0, {0.f, 0.f, -3.f}});
        patrolMotion.path().add_waypoint({5'000, {5.f, 0.f, -3.f}});
        state.patrolMotionPathLength = patrolMotion.path().total_length();
        const fuse::cinematics::MotionSample patrolSample = patrolMotion.sample_at(2'500);
        state.patrolMotionSampleX = patrolSample.position.x;
        static const char* kSeqScrubText =
            "duration_ms=5000\n"
            "sprite hud 0,0,0,1 5000,10,20,1\n"
            "camera 0,0,0,8,55 5000,0,30,12,70\n"
            "actor agent mount 1000 cockpit 10\n";
        fuse::cinematics::Timeline scrubTimeline;
        fuse::cinematics::SeqScrubPreview seqPreview;
        std::string seqPreviewError;
        state.seqScrubPreviewValid =
            fuse::cinematics::scrub_seq_preview(kSeqScrubText, 2'500, scrubTimeline, seqPreview, &seqPreviewError) &&
            seqPreview.valid && seqPreview.has_actor_events;
        static const char* kMotionSeqText = "duration_ms=1000\nmotion path_a 0,0,0,0 1000,5,0,0\n";
        fuse::cinematics::Timeline motionTimeline;
        state.motionTimelineLoaded =
            fuse::cinematics::load_timeline_from_asset(kMotionSeqText, motionTimeline, nullptr);
        state.timeline.pause();
        state.timeline.scrub_to(0, false);
        state.timeline.scrub_to(2'500);
        state.timelineScrubCueCount = state.timeline.cue_queue().pending_count();
        if (state.timelineScrubCueCount > 0u) {
            state.timelineScrubFirstCue = state.timeline.cue_queue().pending()[0].label;
        }
        state.timeline.cue_queue().clear();
        state.timeline.clear_consumed_cues();
        state.timeline.scrub_to(0, false);
        state.timeline.play();
    }
    setupFx(state);

    state.loadedOutpostStub = fuse::adventure::loadEmbeddedOutpostStub(state.outpostContent);
    state.spawnedOutpostInteractables =
        state.loadedOutpostStub &&
        fuse::adventure::spawnOutpostInteractables(state.outpostContent, state.outpostSpawn);
    state.appliedOutpostPlacements =
        state.spawnedOutpostInteractables &&
        fuse::adventure::applyOutpostScenePlacements(state.outpostSpawn, state.guard3D, state.lever3D);
    fuse::adventure::registerOutpostConversationScriptHooks(state.conversationScriptVm);
    {
        fuse::adventure::ConversationScriptHook aggressiveHook;
        aggressiveHook.npcId = "outpost_guard";
        aggressiveHook.branchId = "aggressive";
        aggressiveHook.requiredItem = "security_pass";
        aggressiveHook.minInventoryCount = 1;
        fuse::adventure::InteractContext inventoryProbeCtx;
        inventoryProbeCtx.inventory = &state.playerInventory;
        state.aggressiveInventoryRequirementUnmetBeforePass =
            !state.conversationScriptVm.inventoryMeetsRequirements(aggressiveHook, inventoryProbeCtx);
    }
    static const char* kExtraConvText =
        "branch outpost_guard briefing\n"
        "line Reactor sector is hot.\n";
    {
        std::vector<fuse::adventure::ConversationScriptHook> loadedConvHooks;
        state.convHooksLoaderParsed =
            fuse::adventure::load_conversation_hooks_from_text(kExtraConvText, loadedConvHooks, nullptr) &&
            !loadedConvHooks.empty();
    }
    (void)fuse::adventure::register_conversation_hooks_from_text(kExtraConvText, state.conversationScriptVm,
                                                                nullptr);
    static const char* kPriorityConvText =
        "branch outpost_guard polite\n"
        "line Hello.\n"
        "priority 1\n"
        "branch outpost_guard urgent\n"
        "line Move along.\n"
        "priority 5\n";
    (void)fuse::adventure::register_conversation_hooks_from_text(kPriorityConvText, state.conversationScriptVm,
                                                                nullptr);
    static const char* kElifConvText =
        "branch outpost_guard veteran\n"
        "requires service_medal 1\n"
        "elif requires security_badge 1\n"
        "line Alternate clearance accepted.\n";
    fuse::adventure::ConversationScriptVm elifVm;
    (void)fuse::adventure::register_conversation_hooks_from_text(kElifConvText, elifVm, nullptr);
    {
        fuse::adventure::InteractContext elifCtx;
        elifCtx.inventory = &state.playerInventory;
        state.playerInventory.setMaxLimit(fuse::adventure::ItemId("security_badge"), 1);
        state.playerInventory.incInventory(fuse::adventure::ItemId("security_badge"), 1);
        state.elifVeteranBranchAllowed = elifVm.canDispatchBranch("outpost_guard", "veteran", elifCtx);
    }
    static const char* kTsConvText =
        "function onConversation_outpost_guard_welcome() {\n"
        "  echo(\"Welcome to the outpost.\");\n"
        "}\n";
    {
        std::vector<fuse::adventure::ConversationScriptHook> tsConvHooks;
        state.convTsHooksLoaderParsed =
            fuse::adventure::load_conversation_hooks_from_torquescript(kTsConvText, tsConvHooks, nullptr) &&
            !tsConvHooks.empty();
    }
    state.torquescriptConvHooksRegistered =
        fuse::adventure::register_conversation_hooks_from_torquescript(kTsConvText, state.conversationScriptVm);
    {
        fuse::adventure::InteractContext aggressiveCtx;
        aggressiveCtx.inventory = &state.playerInventory;
        state.guardAggressiveBranchGated =
            !state.conversationScriptVm.canDispatchBranch("outpost_guard", "aggressive", aggressiveCtx);
        state.playerInventory.setMaxLimit(fuse::adventure::ItemId("security_pass"), 1);
        state.playerInventory.incInventory(fuse::adventure::ItemId("security_pass"), 1);
        state.guardAggressiveBranchAllowed =
            state.conversationScriptVm.canDispatchBranch("outpost_guard", "aggressive", aggressiveCtx);
    }
    {
        fuse::adventure::InteractContext priorityCtx;
        priorityCtx.inventory = &state.playerInventory;
        (void)state.conversationScriptVm.chooseHighestPriorityBranch("outpost_guard", priorityCtx,
                                                                   state.conversationPriorityBranch);
    }
    state.aggressiveBranchLineCount =
        state.conversationScriptVm.branchLineCount("outpost_guard", "aggressive");
    state.aggressivePeekLine =
        state.conversationScriptVm.peekBranchLine("outpost_guard", "aggressive", 1);
    state.welcomeBranchLineCount = state.conversationScriptVm.branchLineCount("outpost_guard", "welcome");

    if (state.spawnedOutpostInteractables) {
        state.playerInventory.setMaxLimit(fuse::adventure::ItemId("rusty_key"), 1);
        state.playerInventory.incInventory(fuse::adventure::ItemId("rusty_key"), 1);
        auto doorIt = state.outpostSpawn.doors.find("maintenance_door");
        if (doorIt != state.outpostSpawn.doors.end() && doorIt->second != nullptr) {
            fuse::adventure::InteractContext doorCtx;
            doorCtx.inventory = &state.playerInventory;
            doorCtx.actorName = "player";
            (void)state.adventureSystem.use(doorCtx, fuse::adventure::ItemId("rusty_key"), *doorIt->second);
            state.maintenanceDoorOpened = doorIt->second->isOpen();
        }
    }

    {
        auto guardIt = state.outpostSpawn.conversations.find("outpost_guard");
        if (guardIt != state.outpostSpawn.conversations.end() && guardIt->second != nullptr) {
            guardIt->second->injectScriptLine("Keep your weapon holstered near the reactor.");
            state.conversationInjectedLineCount = guardIt->second->injectedLineCount();
        }
    }

    state.allyMover.setTarget(state.ally3D.x() + 2.f, state.ally3D.y(), state.ally3D.z());
    state.cameraFollow.setPosition(state.lever3D.x(), state.lever3D.y(), state.lever3D.z());
    state.cameraFollow.setTargetObjectId(kAgentObjectId);

    state.reactorZone.setTickPeriodMs(100);
    state.reactorZone.advance(50);
    state.reactorZone.advance(150);
    state.reactorZoneElapsedMs = 150;

    fuse::ai::BehaviorTree patrolLoaderTree;
    static const char* kPatrolLoaderText = "bb.action.set_flag flag=1\nroot=0\n";
    if (fuse::ai::loadTreeFromText(kPatrolLoaderText, patrolLoaderTree, nullptr)) {
        state.patrolLoaderNodeCount = patrolLoaderTree.nodeCount();
        state.aiRuntime.registerTreeProfile(11, std::move(patrolLoaderTree));
    }
    {
        std::vector<fuse::ai::NodeLoadSpec> specs;
        fuse::ai::NodeLoadSpec rootSpec;
        rootSpec.typeId = "bb.root";
        rootSpec.childIndices = {1};
        fuse::ai::NodeLoadSpec flagSpec;
        flagSpec.typeId = "bb.action.set_flag";
        flagSpec.flagIndex = 3;
        specs.push_back(rootSpec);
        specs.push_back(flagSpec);
        fuse::ai::BehaviorTree specsTree;
        if (fuse::ai::loadTreeFromSpecs(specs, 0, specsTree)) {
            state.specsLoaderNodeCount = specsTree.nodeCount();
            state.aiRuntime.registerTreeProfile(14, std::move(specsTree));
        }
    }
    state.treeFileWatchRegistry.watchProfile("hybrid_patrol_mem.bt", 12u, "bb.action.set_flag flag=1\nroot=0\n");
    state.treeFileWatchRegistry.setContent("hybrid_patrol_mem.bt", "bb.action.set_flag flag=2\nroot=0\n");
    state.treeMemoryReloadCount = state.treeFileWatchRegistry.pollReloads(state.aiRuntime);
    state.codegenImportProfileOk = fuse::ai::uaisk::importCodegenProfile(
        "aiTargeting.cs", kTargetingPatrolCs, 13u, state.aiRuntime, nullptr);
    state.codegenReloadProfileOk =
        fuse::ai::uaisk::reloadCodegenProfile("aiTargeting.cs", kTargetingPatrolCs, 13u, state.aiRuntime);
    fuse::ai::wireAgentEntityBindings(state.aiRuntime, {{0u, fuse::Handle<fuse::Object>(1u, 1u)}});
    state.nodeRegistryMoveTowardFactory =
        fuse::ai::NodeRegistry::instance().hasFactory("gb.action.move_toward");
    state.uaiskTemplateHooksNonZero = fuse::ai::uaisk::templateHookCount >= 4u;
    state.useVerbFromActionKind = fuse::mechanics::verbForActionKind(fuse::mechanics::InteractActionKind::Use);
    state.pickupVerbFromActionKind =
        fuse::mechanics::actionKindForVerb("pickup") == fuse::mechanics::InteractActionKind::Pickup;

    state.playerBag.inventory().setMaxLimit(fuse::adventure::ItemId("tool_key"), 1);
    state.playerBag.inventory().incInventory(fuse::adventure::ItemId("tool_key"), 1);
    fuse::adventure::InteractContext toolDoorCtx;
    toolDoorCtx.inventory = &state.playerBag.inventory();
    toolDoorCtx.actorName = "player";
    (void)state.adventureSystem.use(toolDoorCtx, fuse::adventure::ItemId("tool_key"), state.toolRoomDoor);
    state.toolRoomDoorOpened = state.toolRoomDoor.isOpen();
    state.toolDoorOpenMessageOk = state.toolRoomDoor.openMessage() == "Tool room opened";
    state.hudPromptViaPromptFor =
        state.adventureSystem.promptFor(state.hudPrompt) == "Press E to activate lever";

    state.playerInventory.setMaxLimit(fuse::adventure::ItemId("outpost_token"), 1);
    {
        fuse::adventure::PickupInteractable outpostTokenPickup{fuse::adventure::ItemId("outpost_token")};
        fuse::adventure::InteractContext tokenPickupCtx;
        tokenPickupCtx.inventory = &state.playerInventory;
        tokenPickupCtx.actorName = "player";
        state.adventurePickupViaSystem =
            state.adventureSystem.pickup(tokenPickupCtx, fuse::adventure::ItemId("outpost_token"), 1,
                                         outpostTokenPickup) == fuse::adventure::InteractResult::PickedUp &&
            outpostTokenPickup.consumed();
    }

    state.playerInventory.setMaxLimit(fuse::adventure::ItemId("sidearm"), 1);
    state.playerInventory.setMaxLimit(fuse::adventure::ItemId("energy_cell"), 99);
    fuse::adventure::InteractContext sidearmPickupCtx;
    sidearmPickupCtx.inventory = &state.playerInventory;
    sidearmPickupCtx.actorName = "player";
    (void)state.labSidearmPickup.onPickup(sidearmPickupCtx, fuse::adventure::ItemId("sidearm"), 1);
    state.labSidearmPickupConsumed = state.labSidearmPickup.consumed();

    {
        fuse::adventure::ConversationScriptHook aggressiveHook;
        aggressiveHook.npcId = "outpost_guard";
        aggressiveHook.branchId = "aggressive";
        aggressiveHook.requiredItem = "security_pass";
        aggressiveHook.minInventoryCount = 1;
        fuse::adventure::InteractContext inventoryProbeCtx;
        inventoryProbeCtx.inventory = &state.playerInventory;
        state.aggressiveInventoryRequirementMet =
            state.conversationScriptVm.inventoryMeetsRequirements(aggressiveHook, inventoryProbeCtx);
    }

    state.playerBag.inventory().setMaxLimit(fuse::adventure::ItemId("energy_cell"), 99);
    state.playerActor.addComponent(&state.playerBag);
    state.playerActor.addComponent(&state.flareInteractable);
    state.playerActor.attach();
    if (auto* inventoryIface = state.playerActor.getInterface<fuse::mechanics::InventoryProviderInterface>(
            "mechanics", "inventory", &state.playerBag)) {
        state.playerBag.inventory().setMaxLimit(fuse::adventure::ItemId("provider_probe"), 4);
        const fuse::u32 granted = inventoryIface->grantItem("provider_probe", 2);
        const fuse::u32 consumed = inventoryIface->consumeItem("provider_probe", 1);
        state.inventoryProviderInterfaceOk = granted == 2u && consumed == 1u &&
                                             inventoryIface->getItemCount("provider_probe") == 1u;
    }
    state.flareInteractable.attach();
    state.mechanicsRegistry.registerInteractable(&state.flareInteractable, &state.flareInteractable);
    state.registryResolvedInteractable =
        fuse::mechanics::MechanicsRegistry::resolveInteractable(&state.flareInteractable) != nullptr;
    fuse::mechanics::InteractionContext registryProbeCtx;
    registryProbeCtx.instigatorRoot = &state.playerActor;
    registryProbeCtx.verb = "use";
    registryProbeCtx.item = "Flashlight";
    state.registryUseBlockedWithoutItem =
        !state.mechanicsRegistry.canInteract(&state.flareInteractable, registryProbeCtx);

    state.bridgeKeyObject.attach();
    state.bridgeDoorObject.attach();
    state.playerBag.inventory().setMaxLimit(fuse::adventure::ItemId("bridge_key"), 1);
    fuse::mechanics::InteractionContext bridgePickupCtx;
    bridgePickupCtx.item = "bridge_key";
    bridgePickupCtx.amount = 1;
    const fuse::adventure::InteractResult bridgePickupResult = state.mechanicsBridge.pickup(
        state.mechanicsRegistry, &state.bridgeKeyObject, &state.playerActor, bridgePickupCtx);
    fuse::mechanics::InteractionContext bridgeUseCtx;
    bridgeUseCtx.item = "bridge_key";
    const fuse::adventure::InteractResult bridgeUseResult = state.mechanicsBridge.use(
        state.mechanicsRegistry, &state.bridgeDoorObject, &state.playerActor, bridgeUseCtx);
    state.mechanicsBridgeVerticalSlice =
        bridgePickupResult == fuse::adventure::InteractResult::PickedUp &&
        bridgeUseResult == fuse::adventure::InteractResult::Used && state.bridgeDoorGate.isUnlocked();
    state.cellPickupObject.attach();
    state.mechanicsRegistry.registerInteractable(&state.cellPickupObject, &state.cellPickupObject);
    fuse::adventure::QueuedInteraction cellPickupQueued;
    cellPickupQueued.target = &state.cellPickupObject;
    cellPickupQueued.instigator = &state.playerActor;
    cellPickupQueued.ctx.verb = "pickup";
    cellPickupQueued.ctx.item = "energy_cell";
    cellPickupQueued.ctx.amount = 1;
    const fuse::mechanics::InteractAction pickupAction =
        fuse::mechanics::InteractAction::fromContext(cellPickupQueued.ctx);
    state.pickupActionRoundTrip =
        pickupAction.kind == fuse::mechanics::InteractActionKind::Pickup && pickupAction.item == "energy_cell";
    state.interactionQueue.enqueue(cellPickupQueued);
    state.queuedInteractionProcessed = state.interactionQueue.drain(state.mechanicsRegistry, state.mechanicsBridge);

    state.reactorSealObject.attach();
    state.mechanicsRegistry.registerInteractable(&state.reactorSealObject, &state.reactorSealObject);
    fuse::adventure::QueuedInteraction sealUseQueued;
    sealUseQueued.target = &state.reactorSealObject;
    sealUseQueued.instigator = &state.playerActor;
    sealUseQueued.ctx.verb = "use";
    sealUseQueued.ctx.item = "energy_cell";
    state.interactionQueue.enqueue(sealUseQueued);
    state.queuedInteractionProcessed +=
        state.interactionQueue.drain(state.mechanicsRegistry, state.mechanicsBridge);
    state.reactorSealUnlocked = state.reactorSealGate.isUnlocked();

    state.archiveFailObject.attach();
    fuse::adventure::QueuedInteraction archiveFailUse;
    archiveFailUse.target = &state.archiveFailObject;
    archiveFailUse.instigator = &state.playerActor;
    archiveFailUse.ctx.verb = "use";
    archiveFailUse.ctx.item = "archive_key";
    state.interactionQueue.enqueue(archiveFailUse);
    (void)state.interactionQueue.drain(state.mechanicsRegistry, state.mechanicsBridge);
    state.interactionQueueFailedCount = state.interactionQueue.failedCount();
    state.interactionQueue.enqueue(archiveFailUse);
    state.interactionQueue.clear();
    state.interactionQueueClearedPending = state.interactionQueue.pendingCount() == 0u;

    if (state.spawnedOutpostInteractables) {
        auto guardIt = state.outpostSpawn.conversations.find("outpost_guard");
        if (guardIt != state.outpostSpawn.conversations.end() && guardIt->second != nullptr) {
            fuse::adventure::InteractContext scriptCtx;
            scriptCtx.actorName = "player";
            scriptCtx.inventory = &state.playerInventory;
            state.briefingDispatchAllLines = state.conversationScriptVm.dispatchAllLines(
                "outpost_guard", "polite", scriptCtx, *guardIt->second);
            state.playerInventory.setMaxLimit(fuse::adventure::ItemId("security_badge"), 1);
            const bool aggressiveDispatched = state.conversationScriptVm.dispatchBranch(
                "outpost_guard", "aggressive", scriptCtx, *guardIt->second);
            state.conversationGrantCountSnapshot = state.conversationScriptVm.grantCount();
            (void)aggressiveDispatched;
        }
    }

    state.leverInteractable.setSupportedVerbs({"use"});
    state.leverUseStub.attach();
    state.leverPickupStub.attach();
    state.leverExamineStub.attach();
    state.leverInteractable.attach();
    state.mechanicsRegistry.registerInteractable(&state.leverInteractable, &state.leverInteractable);

    // The VActor mount chain above lifts agent_3d to ~(y 2.1, z 2.75); the lever volume must
    // enclose the mounted spawn or the enter event never fires.
    const fuse::mechanics::ConvexPolyhedron triggerVolume =
        fuse::mechanics::ConvexPolyhedron::axis_aligned_box(-5.f, -1.f, -1.f, 4.5f, 2.5f, 3.f);
    state.leverTrigger.setPolyhedron(triggerVolume);
    state.leverConsole.registerMethod("toggleLever", [&state]() {
        state.leverToggle.toggle();
        state.leverSwitch.flip();
    });
    state.leverTrigger.setOnLeave([&state](u32 /*objectId*/) { ++state.polyhedronLeaveCallbackCount; });
    state.leverTrigger.setOnEnter([&state](u32 /*objectId*/) {
        state.agentInsideTrigger = true;
        state.reactorLamp.enable();
        state.outpostCam.activate();
        state.leverRadio.stopBroadcast();
        state.radioStoppedOnLever = !state.leverRadio.broadcasting();
        state.leverConsole.invoke("toggleLever");
        state.leverDelay.advance(100);
        state.leverRotate.advance(1.f / 60.f);
        state.leverCounter.increment();
        state.leverMessage.send();
        (void)state.leverSpawn.triggerSpawn();
        (void)state.leverDestroy.triggerDestroy();
        state.leverSound.play();

        if (state.leverArea.contains(state.agent3D.x(), state.agent3D.y(), state.agent3D.z())) {
            ++state.areaOverlapCount;
        }
        (void)state.leverArea.testObject(kAgentObjectId, state.agent3D.x(), state.agent3D.y(),
                                         state.agent3D.z());

        fuse::mechanics::InteractionContext mechanicsCtx;
        mechanicsCtx.verb = "use";
        const fuse::mechanics::InteractAction leverAction =
            fuse::mechanics::InteractAction::fromContext(mechanicsCtx);
        state.leverUseActionRoundTrip =
            leverAction.kind == fuse::mechanics::InteractActionKind::Use && leverAction.toContext().verb == "use";
        state.leverUseStubExecuted = state.leverUseStub.execute(mechanicsCtx);
        fuse::mechanics::InteractionContext pickupMechanicsCtx;
        pickupMechanicsCtx.verb = "pickup";
        pickupMechanicsCtx.item = "energy_cell";
        state.leverPickupStubExecuted = state.leverPickupStub.execute(pickupMechanicsCtx);
        fuse::mechanics::InteractionContext examineMechanicsCtx;
        examineMechanicsCtx.verb = "examine";
        state.leverExamineStubExecuted = state.leverExamineStub.execute(examineMechanicsCtx);
        state.mechanicsRegistry.interact(&state.leverInteractable, mechanicsCtx);

        fuse::adventure::InteractContext adventureCtx;
        adventureCtx.actorName = "player";
        state.hudPromptText = state.adventureSystem.showHudPrompt(adventureCtx, state.hudPrompt);
        if (state.reactorPlaqueExamineCount == 0u) {
            fuse::adventure::InteractContext examineCtx;
            examineCtx.actorName = "player";
            (void)state.reactorPlaque.onExamine(examineCtx);
            state.reactorPlaqueExamineCount = state.reactorPlaque.examineCount();
            fuse::mechanics::InteractionContext mechanicsExamineCtx;
            mechanicsExamineCtx.verb = "examine";
            const fuse::mechanics::InteractAction examineAction =
                fuse::mechanics::InteractAction::fromContext(mechanicsExamineCtx);
            state.examineActionRoundTrip =
                examineAction.kind == fuse::mechanics::InteractActionKind::Examine &&
                examineAction.toContext().verb == "examine";
        }

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
            grantRequest.stats.magazineSize = 1;
            grantRequest.stats.reloadMs = 50;
            fuse::adventure::SkeletalBoneMount boneMount{};
            boneMount.boneName = "weapon_shoulder";
            boneMount.yawDeg = 15.f;
            state.weaponSkeletalMount.setBoneMount(boneMount);
            state.weaponGranted = state.weaponGrantPipeline.grantOnPickupWithMount(
                grantCtx, *weaponIt->second, grantRequest, state.weaponRuntime, state.weaponMountAnim,
                state.weaponSkeletalMount);
            state.bindPoseBridge.applyMountToBindPose(state.weaponSkeletalMount, state.weaponMountAnim,
                                                      state.bindSkeleton, state.bindPose);
            state.skeletalBoneIndexResolved = state.weaponSkeletalMount.boneMount().boneIndex;
            state.weaponMountAnimApplyCount = state.weaponMountAnim.applyCount();
            state.grantedWeaponDamage = state.weaponRuntime.stats().damage;
            state.weaponFired = state.weaponRuntime.fire(state.playerInventory);
            // Single-round magazine: start the timed reload; the combat loop fires from tickFrame
            // once advanceReload has refilled it.
            if (state.weaponRuntime.magazineAmmo() == 0u) {
                state.weaponReloaded = state.weaponRuntime.reload(state.playerInventory);
            }
        }

        auto guardIt = state.outpostSpawn.conversations.find("outpost_guard");
        if (guardIt != state.outpostSpawn.conversations.end() && guardIt->second != nullptr) {
            fuse::adventure::InteractContext branchCtx;
            branchCtx.actorName = "player";
            branchCtx.inventory = &state.playerInventory;
            if (state.weaponGranted) {
                state.guardPeekBranchLine =
                    state.conversationScriptVm.peekBestBranchLine("outpost_guard", branchCtx);
                state.guardAllLinesDispatchCount = state.conversationScriptVm.dispatchBestBranchAllLines(
                    "outpost_guard", branchCtx, *guardIt->second);
                state.conversationScriptVm.dispatchBestBranch("outpost_guard", branchCtx, *guardIt->second);
                state.guardBranchChosen = state.conversationScriptVm.lastBranchDispatched();
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
            state.conversationScriptVm.setNpcState("outpost_guard", fuse::adventure::ConversationState::Greeting);
            (void)state.conversationScriptVm.advanceNpcState("outpost_guard", branchCtx, *guardIt->second);
            state.guardNpcStateAdvanced =
                state.conversationScriptVm.npcState("outpost_guard") != fuse::adventure::ConversationState::Idle;
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

    fuse::mechanics::BroadphaseWorldBody distantDecoyBody{};
    distantDecoyBody.objectId = 55u;
    distantDecoyBody.proxy =
        fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
    distantDecoyBody.x = 100.f;
    distantDecoyBody.y = 100.f;
    distantDecoyBody.z = 100.f;
    state.broadphaseWorld.addBody(distantDecoyBody);
    state.broadphaseDecoyRemoved = state.broadphaseWorld.removeBody(55u);
    state.broadphaseBodyCountAfterRemove = state.broadphaseWorld.bodyCount();
    state.broadphaseMaskedOverlapHits = state.broadphaseWorld.queryOverlaps(
        fuse::mechanics::BroadphaseProxyFilter::Character, fuse::mechanics::BroadphaseProxyFilter::StaticRigid);

    {
        fuse::mechanics::BroadphaseWorldStub layerProbe;
        fuse::mechanics::BroadphaseWorldBody maskedLayerA{};
        maskedLayerA.objectId = 60u;
        maskedLayerA.proxy =
            fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
        maskedLayerA.collisionLayer = 1u;
        layerProbe.addBody(maskedLayerA);

        fuse::mechanics::BroadphaseWorldBody maskedLayerB{};
        maskedLayerB.objectId = 61u;
        maskedLayerB.proxy =
            fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
        maskedLayerB.collisionLayer = 8u;
        maskedLayerB.collisionMask = 8u;
        layerProbe.addBody(maskedLayerB);
        state.broadphaseLayerMaskedHits = layerProbe.queryOverlaps(
            fuse::mechanics::BroadphaseProxyFilter::Character,
            fuse::mechanics::BroadphaseProxyFilter::Character);
    }
    state.broadphaseProxyFiltersCollide = fuse::mechanics::broadphaseProxyDescsCollide(
        fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character),
        fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Trigger));
    {
        const fuse::mechanics::BroadphaseProxyDesc characterProxy =
            fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
        const fuse::mechanics::BroadphaseProxyDesc triggerProxy =
            fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Trigger);
        state.broadphaseProxyMaskCollideOk = fuse::mechanics::broadphaseProxyMasksCollide(
            characterProxy.group, characterProxy.mask, triggerProxy.group, triggerProxy.mask);
    }

    fuse::mechanics::BtDbvtProxy agentDbvt{};
    agentDbvt.objectId = kAgentObjectId;
    agentDbvt.proxy = fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
    agentDbvt.minX = state.agent3D.x() - 0.5f;
    agentDbvt.maxX = state.agent3D.x() + 0.5f;
    agentDbvt.minY = state.agent3D.y() - 0.5f;
    agentDbvt.maxY = state.agent3D.y() + 0.5f;
    agentDbvt.minZ = state.agent3D.z() - 0.5f;
    agentDbvt.maxZ = state.agent3D.z() + 0.5f;
    state.btDbvtBridge.insertProxy(agentDbvt);

    fuse::mechanics::BtDbvtProxy triggerDbvt{};
    triggerDbvt.objectId = 99u;
    triggerDbvt.proxy = fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Trigger);
    triggerDbvt.minX = state.agent3D.x() - 1.f;
    triggerDbvt.maxX = state.agent3D.x() + 1.f;
    triggerDbvt.minY = state.agent3D.y() - 1.f;
    triggerDbvt.maxY = state.agent3D.y() + 1.f;
    triggerDbvt.minZ = state.agent3D.z() - 1.f;
    triggerDbvt.maxZ = state.agent3D.z() + 1.f;
    state.btDbvtBridge.insertProxy(triggerDbvt);

    state.broadphaseTriggerSync.bindTrigger(&state.leverTrigger);
    state.broadphaseTriggerSync.setPositionProvider([&state](fuse::u32 objectId) -> fuse::mechanics::PhysicsBodyPosition {
        if (objectId == kAgentObjectId) {
            return {state.agent3D.x(), state.agent3D.y(), state.agent3D.z()};
        }
        return {};
    });
    state.broadphaseTriggerSync.trackBody(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());

    state.leverTrigger.testObject(kLeaveSimObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.leverTrigger.testObject(kLeaveSimObjectId, 100.f, 100.f, 100.f);
    (void)state.leverArea.testObject(kLeaveSimObjectId, 0.f, 0.f, 0.f);
    (void)state.leverArea.testObject(kLeaveSimObjectId, 50.f, 0.f, 0.f);

    const auto cuePreview = fuse::cinematics::preview_cues_at(state.timeline, 2'500);
    state.cuePreviewCount = static_cast<fuse::u32>(cuePreview.size());
    const auto midCuePreview = fuse::cinematics::preview_cues_at(state.timeline, 15'000);
    state.cuePreviewMidTimelineCount = static_cast<fuse::u32>(midCuePreview.size());
    fuse::cinematics::Timeline introAssetTimeline;
    state.introTimelineAssetLoaded =
        fuse::cinematics::load_timeline_from_asset(kTimelineHostAssetText, introAssetTimeline, nullptr);
    state.conversationLineDispatches = state.conversationScriptVm.lineDispatchCount();
}

void tickFrame(State& state, fuse::hybrid::HybridComposer& composer, const fuse::frame::FrameCtx& ctx) {
    composer.tick(const_cast<fuse::frame::FrameCtx&>(ctx));

    const fuse::cinematics::TimelineMs deltaMs =
        static_cast<fuse::cinematics::TimelineMs>(std::lround(ctx.dt * 1000.f * kAssetTimeScale));
    const fuse::cinematics::TimelineMs timelineBefore = state.timeline.playhead().time_ms();
    state.timeline.advance(deltaMs);
    const std::vector<fuse::cinematics::CueEntry> drainedCues = state.timeline.cue_queue().drain();
    state.timelineCueDrainCount += static_cast<fuse::u32>(drainedCues.size());
    fuse::cinematics::drain_actor_cues(state.timeline, state.vactorBridge, timelineBefore);
    state.vactorBridge.sync_bound_objects();
    state.vactorBridge.sync_motion_from_timeline(state.timeline);
    state.vactorBridge.sync_bone_attach_from_timeline(state.timeline);
    state.lastTimelineMs = state.timeline.playhead().time_ms();
    if (state.leverOpenPropertyTrack != nullptr) {
        state.leverOpenPropertySample = state.leverOpenPropertyTrack->sample_at(state.lastTimelineMs);
    }

    const fuse::cinematics::HybridTimelineSample drive =
        fuse::cinematics::sample_hybrid_timeline_drive(state.timeline);
    state.hudSprite.setPosition(drive.spriteX, drive.spriteY);
    state.world3D.setClearColor(drive.clearR, drive.clearG, drive.clearB);
    if (drive.spriteX != 0.f || drive.spriteY != 0.f ||
        std::fabs(drive.clearR - state.initialClearR) > 0.001f) {
        state.hybridTimelineDriveSampled = true;
    }

    if (state.aiRuntime.agentCount() >= 3u) {
        state.aiRuntime.setBindingPosition(2u, state.agent3D.x(), state.agent3D.y());
    }
    state.aiRuntime.buildSnapshots();
    {
        std::vector<fuse::ai::AllyCandidate> allies;
        for (fuse::u32 i = 0; i < state.aiRuntime.bindings().size(); ++i) {
            const fuse::ai::AgentBinding& binding = state.aiRuntime.bindings()[i];
            allies.push_back({i, binding.x, binding.y, binding.teamId});
        }
        std::vector<fuse::u32> allyIndices;
        if (!state.aiRuntime.bindings().empty()) {
            const fuse::ai::AgentBinding& self = state.aiRuntime.bindings()[0];
            state.allySpatialFilterCount = fuse::ai::filter_allies_in_radius(
                0u, self.teamId, self.x, self.y, 12.f, allies, allyIndices);
            const fuse::ai::NearestAllyResult nearest =
                fuse::ai::find_nearest_ally(0u, self.teamId, self.x, self.y, allies);
            state.nearestAllyFound = nearest.found;
            state.nearestAllyIndex = nearest.allyIndex;
            state.allyCountInRadius = fuse::ai::count_allies_in_radius(0u, self.teamId, self.x, self.y, 12.f, allies);
            const fuse::ai::NearestAllyResult nearestWithin =
                fuse::ai::find_nearest_ally_within_radius(0u, self.teamId, self.x, self.y, 12.f, allies);
            state.nearestAllyWithinRadiusFound = nearestWithin.found;
            state.nearestAllyWithinRadiusSq = nearestWithin.distanceSq;
            state.alliesOutsideRadius = fuse::ai::count_allies_outside_radius(
                0u, self.teamId, self.x, self.y, 2.f, allies);
            if (allies.size() > 1u) {
                state.allyDistanceSqSample =
                    fuse::ai::distance_sq_2d(self.x, self.y, allies[1].x, allies[1].y);
                state.allyDistanceWithinRadius =
                    fuse::ai::within_radius(state.allyDistanceSqSample, 12.f);
            }
        }

        fuse::ai::ParallelPolicy parallelPolicy;
        fuse::ai::Blackboard board;
        board.resize(1);
        const fuse::ai::BlackboardView boundBoard(board);
        fuse::ai::BehaviorEvalContext parallelCtx;
        parallelCtx.allies = &allies;
        if (!state.blackboardViewBound) {
            bool probeFlag = false;
            board.setFlag(0, 1, true);
            state.blackboardViewBound =
                boundBoard.isBound() && boundBoard.tryGetFlag(0, 1, probeFlag) && probeFlag;
        }
        state.parallelPreconditionsMet =
            fuse::ai::parallel_preconditions_satisfied(parallelPolicy, 0u, boundBoard, parallelCtx);
        fuse::ai::ParallelPolicy thresholdPolicy;
        thresholdPolicy.successThreshold = 1;
        state.parallelPolicyValid = fuse::ai::is_parallel_policy_valid(thresholdPolicy, 2u);
        const fuse::ai::ParallelPolicy normalizedPolicy =
            fuse::ai::normalize_parallel_policy(thresholdPolicy, 2u);
        state.parallelSuccessThreshold =
            fuse::ai::effective_success_threshold(normalizedPolicy, 2u);
        fuse::ai::RadiusFilterPolicy radiusPolicy{12.f, 1};
        state.radiusFilterPolicyValid = fuse::ai::radius_filter_policy_is_valid(radiusPolicy);
        state.effectiveAllyRadius = fuse::ai::effective_radius(radiusPolicy);
        fuse::ai::ParallelPolicy guardedPolicy;
        guardedPolicy.requireBoundBlackboard = true;
        state.parallelPolicyHasGuards = fuse::ai::parallel_policy_has_guards(guardedPolicy);
        state.parallelPolicyTreeValid = fuse::ai::parallel_policy_is_valid(thresholdPolicy, 2u);
        state.parallelFailThreshold = fuse::ai::effective_fail_threshold(thresholdPolicy);
        state.allyRadiusFinite = fuse::ai::is_finite_ally_radius(12.f);
        fuse::ai::RadiusFilterPolicy minCountPolicy{12.f, 2};
        state.effectiveMinAllyCount = fuse::ai::effective_min_count(minCountPolicy);
        state.allyRadiusSqFromPolicy = fuse::ai::radius_sq_from_policy(minCountPolicy);
        state.unlimitedAllyRadiusSentinel = fuse::ai::is_unlimited_radius(0.f);
    }
    state.aiRuntime.evaluate(ctx);
    state.aiRuntime.commit();

    if (!state.aiRuntime.bindings().empty()) {
        const fuse::ai::AgentBinding& binding = state.aiRuntime.bindings()[0];
        state.agent3D.setPosition(binding.x, binding.y);
    }

    state.allyMover.advance(ctx.dt);
    state.cameraFollow.advanceToward(state.agent3D.x(), state.agent3D.y(), state.agent3D.z(), ctx.dt);
    state.reactorZone.testObject(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.reactorZoneElapsedMs += static_cast<fuse::u32>(ctx.dt * 1000.f);
    state.reactorZone.advance(state.reactorZoneElapsedMs);

    state.fxComposer.tick(ctx);
    state.choreographerBridge.tick(state.fxComposer, ctx);
    state.effectGraphCompletedCount = state.fxComposer.effectGraph().completedCount();
    state.missilePipelineCompleted = state.fxComposer.missiles().completedCount();
    for (const fuse::fx::CastInstance& castInstance : state.fxComposer.castPipeline().instances()) {
        if (castInstance.phase == fuse::fx::SpellPhase::Delivery || castInstance.finished) {
            state.castReachedDeliveryPhase = true;
        }
    }
    state.missionScriptVm.dispatchTick(state.fxComposer, ctx);
    state.delayedMissionDispatchCount += state.missionScriptVm.advanceDelayedDispatches(
        static_cast<fuse::u32>(ctx.dt * 1000.f), state.fxComposer, ctx);
    state.weaponRuntime.advanceReload(static_cast<fuse::u32>(ctx.dt * 1000.f));
    if (state.weaponGranted && !state.combatLoopFired) {
        state.weaponCombatLoop.tick(ctx.dt);
        state.combatLoopFired = state.weaponCombatLoop.tryFire(state.playerInventory);
    }
    if (!state.guardHealApplied && state.guardHealth.damageEvents() > 0u) {
        (void)state.guardHealth.heal(10);
        state.guardHealApplied = true;
    }

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
    state.broadphaseAabbHits = state.broadphaseWorld.queryAabbOverlaps(
        state.agent3D.x() - 3.f, state.agent3D.y() - 3.f, state.agent3D.z() - 3.f, state.agent3D.x() + 3.f,
        state.agent3D.y() + 3.f, state.agent3D.z() + 3.f);
    state.btDbvtOverlapHits = state.btDbvtBridge.queryOverlaps(
        fuse::mechanics::BroadphaseProxyFilter::Character, fuse::mechanics::BroadphaseProxyFilter::Trigger);
    state.physicsTriggerBridge.syncObject(kAgentObjectId);
    state.broadphaseTriggerSync.trackBody(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.broadphaseTriggerSync.syncAll();
    state.leverTrigger.testObject(kAgentObjectId, state.agent3D.x(), state.agent3D.y(), state.agent3D.z());
    state.broadphaseRaycastHits = state.broadphaseWorld.queryRaycastStubFiltered(
        state.agent3D.x(), state.agent3D.y(), state.agent3D.z(), 1.f, 0.f, 0.f, 8.f,
        fuse::mechanics::BroadphaseProxyFilter::Character);
    state.broadphaseRaycastStubHits = state.broadphaseWorld.queryRaycastStub(
        state.agent3D.x(), state.agent3D.y(), state.agent3D.z(), 1.f, 0.f, 0.f, 8.f);
}

VerifyResult verify(const State& state, fuse::hybrid::HybridComposer& composer) {
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
    if (state.missionSpellCodegenCount == 0u || state.missionSpellCodegenId != "fireball") {
        return {false, "fuse_fx codegen_spells_from_mission_body in hybrid demo"};
    }
    if (state.treeFileWatchFseventsPollCount == 0u && state.treeFileWatchFseventsReloadCount == 0u) {
        return {false, "fuse_ai TreeFileWatchRegistry pollFSEventsFileChanges progressed in hybrid demo"};
    }
    if (state.guardPeekBranchLine != "I see you are armed. Keep that rifle stowed.") {
        return {false, "fuse_adventure peekBestBranchLine on armed guard in hybrid demo"};
    }
    if (!state.missionSpellAssignmentApplied) {
        return {false, "fuse_fx apply_afx_mission_spell_assignments in hybrid demo"};
    }
    if (state.treeFileWatchOsPollCount == 0u) {
        return {false, "fuse_ai TreeFileWatchRegistry pollOsFileChanges in hybrid demo"};
    }
    if (state.guardAllLinesDispatchCount < 1u) {
        return {false, "fuse_adventure dispatchBestBranchAllLines on guard trigger in hybrid demo"};
    }
    if (state.leverTrigger.leaveCount() == 0u && state.polyhedronLeaveCallbackCount == 0u &&
        state.leverArea.leaveCount() == 0u) {
        return {false, "fuse_mechanics polyhedron setOnLeave and AreaComponent leave in hybrid demo"};
    }
    if (state.timelineCueDrainCount == 0u) {
        return {false, "fuse_cinematics timeline cue_queue drain during hybrid tick"};
    }
    if (state.missionRegisterHookCount < 2u) {
        return {false, "fuse_fx register_afx_mission_from_mis hook table in hybrid demo"};
    }
    if (state.fxComposer.findSpell("fireball") == nullptr) {
        return {false, "fuse_fx register_afx_mission_from_mis registered fireball spell"};
    }
    if (state.conversationInjectedLineCount == 0u) {
        return {false, "fuse_adventure ConversationInteractable injectScriptLine in hybrid demo"};
    }
    if (state.missionNestedSimObjectCount < 2u) {
        return {false, "fuse_fx parse_afx_mission_body_from_mis nested SimObject in hybrid demo"};
    }
    if (state.leverArea.enterCount() == 0u && state.areaOverlapCount == 0u) {
        return {false, "fuse_mechanics AreaComponent contains/test on polyhedron trigger enter"};
    }
    if (state.leverDestroy.destroyCount() == 0u) {
        return {false, "fuse_mechanics DestroyComponent fired on polyhedron trigger enter"};
    }
    if (state.leverSound.playCount() == 0u) {
        return {false, "fuse_mechanics SoundComponent played on polyhedron trigger enter"};
    }
    if (state.timelineScrubCueCount != 1u || state.timelineScrubFirstCue != "door_open") {
        return {false, "fuse_cinematics timeline scrub_to enqueues forward-crossed cues in hybrid demo"};
    }
    if (state.uaiskExpressionNodeCount < 1u) {
        return {false, "fuse_ai UaiskExpressionAst distance profile registered in hybrid demo"};
    }
    if (!state.uaiskExpressionAstValid || state.uaiskExpressionThreshold != 12.f) {
        return {false, "fuse_ai UaiskExpressionAst distance < N condition parsed in hybrid demo"};
    }
    if (state.missionScriptVm.executeCount() == 0u) {
        return {false, "fuse_fx execute_afx_mission_from_mis ran TorqueScript bodies in hybrid demo"};
    }
    if (state.leverSpawn.spawnCount() == 0u) {
        return {false, "fuse_mechanics SpawnComponent fired on polyhedron trigger enter"};
    }
    if (!state.timelineHostWired || state.timelineHostStub.overlayWireCount() == 0u) {
        return {false, "fuse_cinematics TimelineHostStub wired from timeline in hybrid demo"};
    }
    if (state.timelineHostStub.lastOverlaySample().time_ms < 2'000) {
        return {false, "fuse_cinematics TimelineHostStub overlay scrub sample valid"};
    }
    if (state.delayedMissionDispatchCount == 0u && state.missionScriptVm.delayedDispatchCount() == 0u) {
        return {false, "fuse_fx mission schedule delayed VM dispatch fired in hybrid demo"};
    }
    if (state.weaponCombatLoop.hitscanHitCount() == 0u) {
        return {false, "fuse_adventure combat loop hitscan hit applied in hybrid demo"};
    }
    if (state.conversationScriptVm.stateAdvanceCount() == 0u) {
        return {false, "fuse_adventure conversation NPC state machine advanced in hybrid demo"};
    }
    if (state.nestedCompositeNodeCount < 7u) {
        return {false, "fuse_ai nested composite codegen profile registered in hybrid demo"};
    }
    if (state.btDbvtBridge.proxyCount() < 2u || state.btDbvtOverlapHits == 0u) {
        return {false, "fuse_mechanics BtDbvtBridge overlap query hit in hybrid demo"};
    }
    if (!state.weaponReloaded && state.weaponRuntime.reloadCount() == 0u) {
        return {false, "fuse_adventure weapon runtime reload began in hybrid demo"};
    }
    if (state.combatHitscan.fireCount() == 0u) {
        return {false, "fuse_adventure CombatHitscanStub fired in hybrid demo"};
    }
    if (state.setupMountChainDepth < 2u) {
        return {false, "fuse_cinematics ShapeBase mount chain depth applied"};
    }
    if (state.leverRadio.broadcastCount() == 0u) {
        return {false, "fuse_mechanics RadioComponent broadcast in hybrid demo"};
    }
    if (!state.radioStoppedOnLever) {
        return {false, "fuse_mechanics RadioComponent stopBroadcast on lever trigger"};
    }
    if (!state.combatLoopFired || state.weaponCombatLoop.damageApplyCount() == 0u) {
        return {false, "fuse_adventure WeaponCombatLoop applied damage after grant"};
    }
    if (state.guardHealth.damageEvents() == 0u) {
        return {false, "fuse_adventure combat loop damaged guard health"};
    }
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
    if (state.fxComposer.particlePoolGpu().selectiveWritebackCount() == 0u) {
        return {false, "fuse_fx selective CUDA writeback in composer tick"};
    }
    if (state.patrolPath.loopCount() == 0u) {
        return {false, "fuse_mechanics PathComponent looped in hybrid demo"};
    }
    if (state.patrolTimer.callbackFireCount() == 0u) {
        return {false, "fuse_mechanics TimerComponent onFire callback fired"};
    }
    if (state.leverAnimate.cycleCount() == 0u || state.leverAnimate.normalizedTime() <= 0.f) {
        return {false, "fuse_mechanics AnimateComponent advance progress via patrol timer"};
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
    if (state.guardBranchChosen != "armed") {
        return {false, "fuse_adventure conversation VM chose armed priority branch"};
    }
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
    if (!state.maintenanceDoorOpened) {
        return {false, "fuse_adventure maintenance door opened with rusty key"};
    }
    if (state.reactorPlaqueExamineCount == 0u) {
        return {false, "fuse_adventure reactor plaque examined on lever trigger"};
    }
    if (state.patrolLoaderNodeCount == 0u) {
        return {false, "fuse_ai loadTreeFromText profile registered for outpost patrol"};
    }
    if (state.treeMemoryReloadCount == 0u) {
        return {false, "fuse_ai in-memory tree profile hot-reload"};
    }
    if (state.allySpatialFilterCount < 1u) {
        return {false, "fuse_ai spatial_query ally radius filter"};
    }
    if (state.choreographerBridge.attachCount() == 0u || state.choreographerBridge.castCount() == 0u) {
        return {false, "fuse_fx AfxChoreographerBridge attach and cast"};
    }
    if (state.choreographerBridge.tickCount() != static_cast<fuse::u32>(kFrameCount)) {
        return {false, "fuse_fx AfxChoreographerBridge ticked each frame"};
    }
    if (state.fxComposer.castPipeline().completedCount() == 0u) {
        return {false, "fuse_fx cast pipeline completed at least one spell"};
    }
    if (state.fxComposer.effectTimeline().completedCount() == 0u) {
        return {false, "fuse_fx effect timeline finished socket playbacks"};
    }
    if (state.residualSpawnCount == 0u && state.fxComposer.residuals().totalExpired() == 0u) {
        return {false, "fuse_fx residual queue spawn or expiry"};
    }
    if (state.reactorAlarmVolumeSample <= 0.2f || state.reactorAlarmVolumeSample >= 1.0f) {
        return {false, "fuse_cinematics audio track volume sampled mid-ramp"};
    }
    if (state.allyMover.tickCount() == 0u) {
        return {false, "fuse_mechanics MoveComponent advanced in hybrid demo"};
    }
    if (state.cameraFollow.tickCount() == 0u) {
        return {false, "fuse_mechanics FollowComponent tracked agent in hybrid demo"};
    }
    if (state.reactorZone.tickCount() == 0u) {
        return {false, "fuse_mechanics TriggerZoneComponent periodic tick"};
    }
    if (state.conversationScriptVm.dispatchCount() == 0u) {
        return {false, "fuse_adventure conversation script VM dispatched branch"};
    }
    if (state.queuedInteractionProcessed < 2u || !state.reactorSealUnlocked) {
        return {false, "fuse_adventure InteractionQueue pickup and PuzzleGate use via MechanicsBridge"};
    }
    if (state.effectGraphCompletedCount < 2u) {
        return {false, "fuse_fx EffectGraph parent/child nodes completed"};
    }
    if (state.missilePipelineCompleted == 0u) {
        return {false, "fuse_fx MissilePipeline completed hybrid outpost shot"};
    }
    if (state.treeInotifyPollCount == 0u) {
        return {false, "fuse_ai TreeFileWatchRegistry pollInotifyFileChanges exercised"};
    }
    if (state.broadphaseMaskedOverlapHits != 0u) {
        return {false, "fuse_mechanics BroadphaseWorldStub masked filter pair returned no false overlaps"};
    }
    if (!state.nearestAllyFound) {
        return {false, "fuse_ai find_nearest_ally located squad ally"};
    }
    if (!state.parallelPreconditionsMet) {
        return {false, "fuse_ai parallel_preconditions_satisfied for default policy"};
    }
    if (!state.leverUseActionRoundTrip) {
        return {false, "fuse_mechanics InteractAction round-trip on lever use verb"};
    }
    if (state.leverOpenPropertyMidRampSample <= 0.f || state.leverOpenPropertyMidRampSample >= 1.f) {
        return {false, "fuse_cinematics PropertyTrack sampled lever open mid-ramp"};
    }
    if (state.combinedMountEuler.yaw_deg < 17.4f || state.combinedMountEuler.pitch_deg < 1.9f) {
        return {false, "fuse_cinematics combine_mount_euler_deg applied mount offsets"};
    }
    if (state.combinedMountYawDeg < 17.4f) {
        return {false, "fuse_cinematics combine_mount_yaw_deg applied mount offsets"};
    }
    if (state.missionEffectCodegenCount < 2u) {
        return {false, "fuse_fx codegen_effects_from_mission_body nested SimObject"};
    }
    if (state.parameterBindSlotCount < 3u) {
        return {false, "fuse_fx ParameterBinder cast slots bound"};
    }
    if (!state.socketConstraintRemapped) {
        return {false, "fuse_fx SocketConstraintManager remapped spark socket"};
    }
    if (state.reactorLamp.enableCount() == 0u || !state.reactorLamp.enabled()) {
        return {false, "fuse_mechanics LightComponent enabled on lever trigger"};
    }
    if (state.outpostCam.activateCount() == 0u || !state.outpostCam.active()) {
        return {false, "fuse_mechanics CameraComponent activated on lever trigger"};
    }
    if (state.allyCountInRadius < 1u) {
        return {false, "fuse_ai count_allies_in_radius"};
    }
    if (!state.nearestAllyWithinRadiusFound) {
        return {false, "fuse_ai find_nearest_ally_within_radius"};
    }
    if (state.broadphaseAabbHits == 0u) {
        return {false, "fuse_mechanics BroadphaseWorldStub AABB query hit agent"};
    }
    if (state.broadphaseLayerMaskedHits != 0u) {
        return {false, "fuse_mechanics collision layer mask suppressed character overlap"};
    }
    if (!state.guardAggressiveBranchGated || !state.guardAggressiveBranchAllowed) {
        return {false, "fuse_adventure canDispatchBranch security_pass gating"};
    }
    if (!state.torquescriptConvHooksRegistered) {
        return {false, "fuse_adventure register_conversation_hooks_from_torquescript"};
    }
    if (!state.pickupActionRoundTrip) {
        return {false, "fuse_mechanics InteractAction pickup verb round-trip"};
    }
    if (state.conversationPriorityBranch != "urgent") {
        return {false, "fuse_adventure chooseHighestPriorityBranch urgent wins"};
    }
    if (state.guardHealth.currentHealth() >= state.guardHealth.maxHealth() ||
        !state.guardHealth.isAlive()) {
        return {false, "fuse_mechanics HealthComponent recorded combat damage"};
    }
    if (!state.seqScrubPreviewValid) {
        return {false, "fuse_cinematics scrub_seq_preview loaded actor mount sample"};
    }
    if (!state.motionTimelineLoaded) {
        return {false, "fuse_cinematics load_timeline_from_asset motion rail"};
    }
    if (state.patrolMotionSampleX < 2.f || state.patrolMotionSampleX > 3.f) {
        return {false, "fuse_cinematics MotionTrack sampled patrol midpoint"};
    }
    if (state.combinedMountQuatYawDeg < 17.f) {
        return {false, "fuse_cinematics combine_mount_orientation yaw"};
    }
    if (!state.constraintSpecParsed) {
        return {false, "fuse_fx parse_constraint_spec point:impact"};
    }
    if (!state.parallelPolicyValid || state.parallelSuccessThreshold != 1u) {
        return {false, "fuse_ai normalize_parallel_policy effective threshold"};
    }
    if (state.alliesOutsideRadius < 1u) {
        return {false, "fuse_ai count_allies_outside_radius"};
    }
    if (!state.radiusFilterPolicyValid || state.effectiveAllyRadius < 11.f) {
        return {false, "fuse_ai RadiusFilterPolicy effective_radius"};
    }
    if (state.aggressiveBranchLineCount < 2u ||
        state.aggressivePeekLine != "You may pass — this time.") {
        return {false, "fuse_adventure peekBranchLine aggressive second line"};
    }
    if (state.welcomeBranchLineCount < 1u) {
        return {false, "fuse_adventure TorqueScript welcome branch line count"};
    }
    if (!state.examineActionRoundTrip) {
        return {false, "fuse_mechanics InteractAction examine verb round-trip"};
    }
    if (!state.broadphaseDecoyRemoved || state.broadphaseBodyCountAfterRemove < 1u) {
        return {false, "fuse_mechanics BroadphaseWorldStub removeBody"};
    }
    if (!state.codegenImportProfileOk || !state.codegenReloadProfileOk) {
        return {false, "fuse_ai importCodegenProfile and reloadCodegenProfile"};
    }
    if (state.briefingDispatchAllLines < 1u) {
        return {false, "fuse_adventure dispatchAllLines polite branch"};
    }
    if (state.conversationGrantCountSnapshot == 0u ||
        !state.playerInventory.hasInventory(fuse::adventure::ItemId("security_badge"))) {
        return {false, "fuse_adventure dispatchBranch aggressive grant security_badge"};
    }
    if (!state.parallelPolicyHasGuards) {
        return {false, "fuse_ai parallel_policy_has_guards"};
    }
    if (state.broadphaseRaycastStubHits == 0u) {
        return {false, "fuse_mechanics BroadphaseWorldStub queryRaycastStub"};
    }
    if (!state.castBeginOk) {
        return {false, "fuse_fx FxComposer beginCast fireball"};
    }
    if (state.cuePreviewMidTimelineCount == 0u) {
        return {false, "fuse_cinematics preview_cues_at mid-timeline"};
    }
    if (!state.leverUseStubExecuted || state.leverUseStub.executionCount() == 0u) {
        return {false, "fuse_mechanics UseActionStub executed on lever trigger"};
    }
    if (!state.guardHealApplied || state.guardHealth.healEvents() == 0u) {
        return {false, "fuse_mechanics HealthComponent heal after combat damage"};
    }
    if (state.useVerbFromActionKind != "use") {
        return {false, "fuse_mechanics verbForActionKind Use"};
    }
    if (!state.toolRoomDoorOpened) {
        return {false, "fuse_adventure DoorInteractable opened with tool_key"};
    }
    if (!state.labSidearmPickupConsumed ||
        !state.playerInventory.hasInventory(fuse::adventure::ItemId("sidearm"))) {
        return {false, "fuse_adventure WeaponPickupInteractable granted sidearm"};
    }
    if (!state.leverPickupStubExecuted) {
        return {false, "fuse_mechanics PickupActionStub executed on lever trigger"};
    }
    if (!state.aggressiveInventoryRequirementUnmetBeforePass || !state.aggressiveInventoryRequirementMet) {
        return {false, "fuse_adventure inventoryMeetsRequirements security_pass gating"};
    }
    if (!state.parallelPolicyTreeValid || state.parallelFailThreshold != 1u) {
        return {false, "fuse_ai parallel_policy_is_valid and effective_fail_threshold"};
    }
    if (!state.allyRadiusFinite) {
        return {false, "fuse_ai is_finite_ally_radius"};
    }
    if (!state.pickupVerbFromActionKind) {
        return {false, "fuse_mechanics actionKindForVerb pickup"};
    }
    if (state.mountEulerRoundTripYaw < 17.f) {
        return {false, "fuse_cinematics euler_deg_to_quaternion round-trip yaw"};
    }
    if (!state.castReachedDeliveryPhase) {
        return {false, "fuse_fx cast pipeline reached delivery or finished"};
    }
    if (!state.guardNpcStateAdvanced) {
        return {false, "fuse_adventure conversation NPC state advanced from greeting"};
    }
    if (state.codegenCsTreeNodeCount < 1u || !state.uaiskCsModuleParsed) {
        return {false, "fuse_ai codegenTreeFromCs and parseCsModule"};
    }
    if (state.allyDistanceSqSample <= 0.f || state.effectiveMinAllyCount < 2u) {
        return {false, "fuse_ai distance_sq_2d and effective_min_count"};
    }
    if (state.allyRadiusSqFromPolicy < 140.f) {
        return {false, "fuse_ai radius_sq_from_policy"};
    }
    if (!state.broadphaseProxyFiltersCollide) {
        return {false, "fuse_mechanics broadphaseProxyDescsCollide character/trigger"};
    }
    if (!state.leverExamineStubExecuted) {
        return {false, "fuse_mechanics ExamineActionStub executed on lever trigger"};
    }
    if (!state.elifVeteranBranchAllowed) {
        return {false, "fuse_adventure elif requires security_badge veteran branch"};
    }
    if (state.conversationLineDispatches == 0u) {
        return {false, "fuse_adventure conversation VM lineDispatchCount"};
    }
    if (!state.toolDoorOpenMessageOk) {
        return {false, "fuse_adventure DoorInteractable open message"};
    }
    if (state.skeletalBoneIndexResolved == 0u || state.weaponMountAnimApplyCount == 0u) {
        return {false, "fuse_adventure skeletal mount bone index and mount animation apply"};
    }
    if (!state.shapeConstraintParsed) {
        return {false, "fuse_fx parse_constraint_spec shape:muzzle"};
    }
    if (!state.introTimelineAssetLoaded) {
        return {false, "fuse_cinematics load_timeline_from_asset intro host asset"};
    }
    if (state.interactionQueueFailedCount == 0u || !state.interactionQueueClearedPending) {
        return {false, "fuse_adventure InteractionQueue failedCount and clear"};
    }
    if (state.patrolMotionPathLength <= 0.f) {
        return {false, "fuse_cinematics MotionPath total_length"};
    }
    if (!state.unlimitedAllyRadiusSentinel) {
        return {false, "fuse_ai is_unlimited_radius sentinel"};
    }
    if (state.grantedWeaponDamage < 20.f) {
        return {false, "fuse_adventure WeaponRuntime stats damage after grant"};
    }
    if (!state.mechanicsBridgeVerticalSlice) {
        return {false, "fuse_adventure MechanicsBridge pickup/use vertical slice"};
    }
    if (!state.registryResolvedInteractable || !state.registryUseBlockedWithoutItem) {
        return {false, "fuse_mechanics MechanicsRegistry resolveInteractable and canInteract gating"};
    }
    if (!state.missionImpactHookDispatched) {
        return {false, "fuse_fx AfxMissionScriptVm on_impact_fx dispatch"};
    }
    if (!state.missionSpellReadyDispatched) {
        return {false, "fuse_fx AfxMissionScriptVm on_spell_ready dispatch"};
    }
    if (state.missionScriptVm.tickDispatchCount() == 0u) {
        return {false, "fuse_fx AfxMissionScriptVm on_tick dispatchTick in hybrid demo"};
    }
    if (!state.missionExecuteImpactViaBody) {
        return {false, "fuse_fx AfxMissionScriptVm executeFunctionBody impact dispatch"};
    }
    if (!state.inventoryProviderInterfaceOk) {
        return {false, "fuse_mechanics InventoryProviderInterface grant/consume via player bag"};
    }
    if (!state.eventTrackScriptHookOk) {
        return {false, "fuse_cinematics EventTrack script_hook_id on hybrid timeline"};
    }
    if (!state.convHooksLoaderParsed) {
        return {false, "fuse_adventure load_conversation_hooks_from_text"};
    }
    if (!state.blackboardScalarRoundTrip) {
        return {false, "fuse_ai Blackboard trySetScalar/tryGetScalar round trip"};
    }
    if (!state.directHitscanOk) {
        return {false, "fuse_adventure CombatHitscanStub direct fireHitscan"};
    }
    if (state.specsLoaderNodeCount < 2u) {
        return {false, "fuse_ai loadTreeFromSpecs registered hybrid profile"};
    }
    if (!state.hudPromptViaPromptFor) {
        return {false, "fuse_adventure InteractionSystem promptFor HUD interactable"};
    }
    if (!state.convTsHooksLoaderParsed) {
        return {false, "fuse_adventure load_conversation_hooks_from_torquescript"};
    }
    if (!state.nodeRegistryMoveTowardFactory) {
        return {false, "fuse_ai NodeRegistry gb.action.move_toward factory"};
    }
    if (!state.uaiskTemplateHooksNonZero) {
        return {false, "fuse_ai uaisk template hook table"};
    }
    if (!state.broadphaseProxyMaskCollideOk) {
        return {false, "fuse_mechanics broadphaseProxyMasksCollide"};
    }
    if (!state.blackboardViewBound) {
        return {false, "fuse_ai BlackboardView tryGetFlag bound read"};
    }
    if (!state.adventurePickupViaSystem) {
        return {false, "fuse_adventure InteractionSystem pickup PickupInteractable"};
    }
    if (state.missionMisHookParseCount < 5u) {
        return {false, "fuse_fx load_afx_mission_hooks_from_mis hybrid .mis hook table"};
    }
    if (!state.missionScheduledPendingDelayed) {
        return {false, "fuse_fx mission schedule pendingDelayedCount after dispatch"};
    }
    if (!state.hybridTimelineDriveSampled) {
        return {false, "fuse_cinematics sample_hybrid_timeline_drive each frame"};
    }
    if (state.fxComposer.findEffect("spark_burst") == nullptr) {
        return {false, "fuse_fx demo vertical slice spark_burst effect"};
    }
    if (state.world2D.readSnapshot().sprites().size() != 1u) {
        return {false, "2D snapshot built via hierarchy fillSnapshotSoA"};
    }
    if (state.world3D.readSnapshot().objects().size() < 4u) {
        return {false, "3D snapshot includes agent + ally + guard + lever objects"};
    }
#if defined(FUSE_HAS_VULKAN_RHI)
    // E03: with a Vulkan device the frame is GPU-rendered (SceneRenderer + sprite / UI layer): check the read-back
    // frame instead of the PlaceholderRenderer, which stays the no-device fallback below.
    if (composer.gpuSceneActive()) {
        HybridSceneRenderer* gpu = composer.gpuScene();
        if (gpu == nullptr || !gpu->waitIdle() || composer.gpuSceneFrames() == 0u) {
            return {false, "GPU scene frames rendered"};
        }
        const OverlayReadbackCheck overlay = checkOverlayReadback(*gpu);
        if (!overlay.valid || overlay.covered == 0u || overlay.mismatched != 0u) {
            return {false, "spinning 2D sprite + HUD quad visible in the GPU readback"};
        }
        if (gpu->lastFrame().instances != 2u || !gpu->lastFrame().uiStageRan) {
            return {false, "GPU frame rendered the floor + lever meshes and composited the UI stage"};
        }
        u8 rgb[3] = {0, 0, 0};
        if (!readbackPixel(*gpu, 2u, 2u, rgb) || (rgb[0] == 0u && rgb[1] == 0u && rgb[2] == 0u)) {
            return {false, "3D frame (sky) present in the GPU readback"};
        }
        return {true, nullptr};
    }
#endif
    if (composer.renderer().sample(160, 120) == 0) {
        return {false, "3D clear colour present"};
    }
    if (composer.renderer().sample(172, 132) == 0) {
        return {false, "spinning 2D sprite visible"};
    }
    return {true, nullptr};
}

} // namespace fuse::hybrid::gates
