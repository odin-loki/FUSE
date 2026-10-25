#pragma once

#include <fuse/adventure/animation_bind_pose_bridge.hpp>
#include <fuse/adventure/hud_prompt_interactable.hpp>
#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/conversation_script_loader.hpp>
#include <fuse/adventure/door_interactable.hpp>
#include <fuse/adventure/weapon_pickup_interactable.hpp>
#include <fuse/adventure/conversation_script_vm.hpp>
#include <fuse/adventure/adventure_interactable_component.hpp>
#include <fuse/adventure/examine_interactable.hpp>
#include <fuse/adventure/interaction_queue.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/inventory_component.hpp>
#include <fuse/adventure/mechanics_bridge.hpp>
#include <fuse/adventure/pickup_interactable.hpp>
#include <fuse/adventure/puzzle_gate.hpp>
#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/outpost_spawn.hpp>
#include <fuse/adventure/skeletal_mount_stub.hpp>
#include <fuse/adventure/weapon_grant_pipeline.hpp>
#include <fuse/adventure/weapon_mount_animation.hpp>
#include <fuse/adventure/combat_hitscan_stub.hpp>
#include <fuse/adventure/weapon_combat_loop.hpp>
#include <fuse/adventure/weapon_runtime.hpp>
#include <fuse/ai/agent_entity_bind.hpp>
#include <fuse/ai/uaisk_cs_codegen.hpp>
#include <fuse/ai/uaisk_cs_parser.hpp>
#include <fuse/ai/spatial_query.hpp>
#include <fuse/ai/tree_loader.hpp>
#include <fuse/ai/uaisk_tree_reload.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/node_registry.hpp>
#include <fuse/ai/parallel_policy.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/cinematics/timeline_host_stub.hpp>
#include <fuse/dimension/world_handle.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/fx/afx_mission_script_vm.hpp>
#include <fuse/fx/afx_choreographer_bridge.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/mechanics/broadphase_trigger_sync.hpp>
#include <fuse/mechanics/broadphase_world_stub.hpp>
#include <fuse/mechanics/bt_dbvt_bridge.hpp>
#include <fuse/mechanics/console_method_component.hpp>
#include <fuse/mechanics/counter_component.hpp>
#include <fuse/mechanics/destroy_component.hpp>
#include <fuse/mechanics/camera_component.hpp>
#include <fuse/mechanics/follow_component.hpp>
#include <fuse/mechanics/light_component.hpp>
#include <fuse/mechanics/move_component.hpp>
#include <fuse/mechanics/delay_component.hpp>
#include <fuse/mechanics/inventory_provider.hpp>
#include <fuse/mechanics/interact_action.hpp>
#include <fuse/mechanics/interactable.hpp>
#include <fuse/mechanics/message_component.hpp>
#include <fuse/mechanics/action_stub.hpp>
#include <fuse/mechanics/animate_component.hpp>
#include <fuse/mechanics/area_component.hpp>
#include <fuse/mechanics/health_component.hpp>
#include <fuse/mechanics/look_at_component.hpp>
#include <fuse/mechanics/radio_component.hpp>
#include <fuse/mechanics/path_component.hpp>
#include <fuse/mechanics/waypoint_component.hpp>
#include <fuse/mechanics/physics_broadphase_bridge.hpp>
#include <fuse/mechanics/timer_component.hpp>
#include <fuse/mechanics/physics_trigger_bridge.hpp>
#include <fuse/mechanics/polyhedron_trigger.hpp>
#include <fuse/mechanics/registry.hpp>
#include <fuse/mechanics/rotate_component.hpp>
#include <fuse/mechanics/sound_component.hpp>
#include <fuse/mechanics/spawn_component.hpp>
#include <fuse/mechanics/switch_component.hpp>
#include <fuse/mechanics/toggle_component.hpp>
#include <fuse/mechanics/trigger_zone.hpp>
#include <fuse/cinematics/cue_preview.hpp>
#include <fuse/cinematics/motion_track.hpp>
#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/cinematics/mount_orientation.hpp>
#include <fuse/cinematics/property_track.hpp>
#include <fuse/cinematics/vactor_bridge.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <memory>
#include <string>

#if __has_include(<fuse/script/script_host.hpp>)
#include <fuse/ai/uaisk_script_host_bridge.hpp>
#include <fuse/script/script_host.hpp>
#define FUSE_HYBRID_GATES_SCRIPT 1
#else
#define FUSE_HYBRID_GATES_SCRIPT 0
#endif

namespace fuse::hybrid::gates {

constexpr int kFrameCount = 60;
constexpr float kDt = 1.f / 60.f;
constexpr fuse::cinematics::TimelineMs kTimelineDurationMs = 1'000;

/// Outpost vertical-slice wiring shared by demo_hybrid_hud and headless integration tests.
struct State {
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    fuse::SceneObject2D hudSprite{"hud_sprite"};
    fuse::SceneObject3D agent3D{"agent_3d"};
    fuse::SceneObject3D ally3D{"ally_3d"};
    fuse::SceneObject3D guard3D{"outpost_guard"};
    fuse::SceneObject3D lever3D{"lever_3d"};

    fuse::ai::BehaviorRuntime aiRuntime;
#if FUSE_HYBRID_GATES_SCRIPT
    fuse::script::ScriptHost scriptHost;
    fuse::ai::uaisk::ScriptHostBridge scriptHostBridge{scriptHost, aiRuntime};
#endif
    fuse::cinematics::Timeline timeline;
    fuse::fx::FxComposer fxComposer;
    fuse::fx::AfxMissionScriptVm missionScriptVm;
    fuse::fx::AfxChoreographerBridge choreographerBridge;

    fuse::mechanics::MechanicsRegistry mechanicsRegistry;
    fuse::mechanics::InteractableComponent leverInteractable{"lever_interactable"};
    fuse::mechanics::UseActionStub leverUseStub{"lever_use_stub"};
    fuse::mechanics::PickupActionStub leverPickupStub{"lever_pickup_stub"};
    fuse::mechanics::ExamineActionStub leverExamineStub{"lever_examine_stub"};
    fuse::mechanics::ToggleComponent leverToggle{"lever_toggle"};
    fuse::mechanics::SwitchComponent leverSwitch{"lever_switch", false, "armed", "safe"};
    fuse::mechanics::ConsoleMethodComponent leverConsole{"lever_console"};
    fuse::mechanics::DelayComponent leverDelay{"lever_delay", 100};
    fuse::mechanics::RotateComponent leverRotate{"lever_rotate", 45.f};
    fuse::mechanics::CounterComponent leverCounter{"lever_counter", 0, 1};
    fuse::mechanics::MessageComponent leverMessage{"lever_message", "Lever activated"};
    fuse::mechanics::SpawnComponent leverSpawn{"lever_spawn", "outpost_guard_spawn"};
    fuse::mechanics::DestroyComponent leverDestroy{"lever_destroy"};
    fuse::mechanics::SoundComponent leverSound{"lever_sound", "Audio/lever_click"};
    fuse::mechanics::AreaComponent leverArea{"lever_area", 12.f};
    fuse::mechanics::PolyhedronTriggerZone leverTrigger;
    fuse::mechanics::PhysicsTriggerBridge physicsTriggerBridge;
    fuse::mechanics::BroadphaseTriggerSync broadphaseTriggerSync;
    fuse::mechanics::PhysicsBroadphaseBridge physicsBroadphaseBridge;
    fuse::mechanics::PathComponent patrolPath{"outpost_patrol"};
    fuse::mechanics::TimerComponent patrolTimer{"outpost_patrol_timer", 0.25f};
    fuse::mechanics::AnimateComponent leverAnimate{"lever_animate", 0.5f};
    fuse::mechanics::WaypointComponent patrolWaypoint{"outpost_patrol_wp", 4.f, 0.f, 0.f};
    fuse::mechanics::LookAtComponent guardLookAt{"guard_look_at", 120.f};
    fuse::mechanics::RadioComponent leverRadio{"lever_radio", "outpost_alert"};
    fuse::mechanics::HealthComponent guardHealth{"outpost_guard_health", 100};
    fuse::mechanics::BroadphaseWorldStub broadphaseWorld;
    fuse::mechanics::BtDbvtBridge btDbvtBridge;
    fuse::mechanics::MoveComponent allyMover{"ally_nudge", 0.08f};
    fuse::mechanics::FollowComponent cameraFollow{"camera_follow", 2.5f};
    fuse::mechanics::TriggerZoneComponent reactorZone{
        "reactor_zone",
        {-6.f, -2.f, -2.f, 5.f, 3.f, 4.f},
    };
    fuse::mechanics::LightComponent reactorLamp{"reactor_lamp", 1.5f};
    fuse::mechanics::CameraComponent outpostCam{"outpost_cam", 70.f};

    fuse::ai::uaisk::TreeFileWatchRegistry treeFileWatchRegistry;

    fuse::adventure::Inventory playerInventory;
    fuse::adventure::CombatHitscanStub combatHitscan;
    fuse::adventure::WeaponCombatLoop weaponCombatLoop;
    fuse::adventure::WeaponGrantPipeline weaponGrantPipeline;
    fuse::adventure::WeaponRuntime weaponRuntime;
    fuse::adventure::WeaponMountAnimationStub weaponMountAnim;
    fuse::adventure::SkeletalMountStub weaponSkeletalMount;
    fuse::adventure::AnimationBindPoseBridge bindPoseBridge;
    fuse::animation::Skeleton bindSkeleton;
    fuse::animation::PoseSoA bindPose;
    fuse::adventure::InteractionSystem adventureSystem;
    fuse::adventure::MechanicsBridge mechanicsBridge;
    fuse::adventure::InteractionQueue interactionQueue;
    fuse::mechanics::Component playerActor{"player"};
    fuse::adventure::InventoryComponent playerBag{"player_bag"};
    fuse::adventure::InteractableStub flareStub;
    fuse::adventure::AdventureInteractableComponent flareInteractable{"flare", flareStub, adventureSystem};
    fuse::adventure::PickupInteractable bridgeKeyPickup{fuse::adventure::ItemId("bridge_key")};
    fuse::adventure::AdventureInteractableComponent bridgeKeyObject{"bridge_key", bridgeKeyPickup,
                                                                    adventureSystem};
    fuse::adventure::PuzzleGate bridgeDoorGate{fuse::adventure::ItemId("bridge_key")};
    fuse::adventure::AdventureInteractableComponent bridgeDoorObject{"bridge_door", bridgeDoorGate,
                                                                     adventureSystem};
    fuse::adventure::PickupInteractable cellPickup{fuse::adventure::ItemId("energy_cell")};
    fuse::adventure::AdventureInteractableComponent cellPickupObject{"cell_pickup", cellPickup,
                                                                     adventureSystem};
    fuse::adventure::DoorInteractable toolRoomDoor{fuse::adventure::ItemId("tool_key"), "Tool room opened"};
    fuse::adventure::WeaponPickupInteractable labSidearmPickup{fuse::adventure::ItemId("sidearm"),
                                                                 fuse::adventure::ItemId("energy_cell"), 5};
    fuse::adventure::PuzzleGate archiveFailGate{fuse::adventure::ItemId("archive_key")};
    fuse::adventure::AdventureInteractableComponent archiveFailObject{"archive_fail", archiveFailGate,
                                                                      adventureSystem};
    fuse::adventure::PuzzleGate reactorSealGate{fuse::adventure::ItemId("energy_cell")};
    fuse::adventure::AdventureInteractableComponent reactorSealObject{"reactor_seal", reactorSealGate,
                                                                      adventureSystem};
    fuse::adventure::ConversationScriptVm conversationScriptVm;
    fuse::adventure::HudPromptInteractable hudPrompt{"Press E to activate lever"};
    fuse::adventure::ExamineInteractable reactorPlaque{
        "Maintenance access panel — reactor sector clearance required."};
    fuse::adventure::OutpostStubContent outpostContent;
    fuse::adventure::OutpostSpawnBundle outpostSpawn;

    fuse::cinematics::VActorBridge vactorBridge;
    fuse::cinematics::TimelineHostStub timelineHostStub;
    fuse::cinematics::TimelineMs lastTimelineMs = 0;
    fuse::cinematics::PropertyTrack* leverOpenPropertyTrack = nullptr;

    std::string hudPromptText;
    std::string guardLineText;
    std::string guardBranchChosen; ///< Branch picked by dispatchBestBranch (advanceNpcState dispatches follow-ups)
    bool agentInsideTrigger = false;
    bool loadedOutpostStub = false;
    bool spawnedOutpostInteractables = false;
    bool appliedOutpostPlacements = false;
    bool weaponGranted = false;
    u32 cuePreviewCount = 0;
    u32 setupMountChainDepth = 0; ///< Depth right after the setup mount chain (timeline cues remount later)
    u32 broadphaseRaycastHits = 0;
    u32 broadphaseDbvtHits = 0;
    bool weaponFired = false;
    bool combatLoopFired = false;
    bool weaponReloaded = false;
    bool timelineHostWired = false;
    u32 nestedCompositeNodeCount = 0;
    u32 uaiskExpressionNodeCount = 0;
    float uaiskExpressionThreshold = 0.f;
    bool uaiskExpressionAstValid = false;
    u32 btDbvtOverlapHits = 0;
    u32 delayedMissionDispatchCount = 0;
    u32 timelineScrubCueCount = 0;
    std::string timelineScrubFirstCue;
    u32 missionSpellCodegenCount = 0;
    std::string missionSpellCodegenId;
    u32 missionNestedSimObjectCount = 0;
    u32 areaOverlapCount = 0;
    u32 conversationInjectedLineCount = 0;
    u32 treeFileWatchFseventsPollCount = 0;
    u32 treeFileWatchFseventsReloadCount = 0;
    u32 missionRegisterHookCount = 0;
    u32 timelineCueDrainCount = 0;
    u32 polyhedronLeaveCallbackCount = 0;
    u32 guardAllLinesDispatchCount = 0;
    u32 treeFileWatchOsPollCount = 0;
    bool missionSpellAssignmentApplied = false;
    std::string guardPeekBranchLine;
    bool maintenanceDoorOpened = false;
    u32 reactorPlaqueExamineCount = 0;
    u32 patrolLoaderNodeCount = 0;
    u32 treeMemoryReloadCount = 0;
    u32 allySpatialFilterCount = 0;
    u32 residualSpawnCount = 0;
    float reactorAlarmVolumeSample = 0.f;
    u32 reactorZoneElapsedMs = 0;
    u32 queuedInteractionProcessed = 0;
    bool reactorSealUnlocked = false;
    u32 missionEffectCodegenCount = 0;
    u32 parameterBindSlotCount = 0;
    bool socketConstraintRemapped = false;
    u32 allyCountInRadius = 0;
    bool nearestAllyWithinRadiusFound = false;
    float nearestAllyWithinRadiusSq = 0.f;
    float combinedMountYawDeg = 0.f;
    u32 broadphaseAabbHits = 0;
    u32 broadphaseLayerMaskedHits = 0;
    bool guardAggressiveBranchGated = false;
    bool guardAggressiveBranchAllowed = false;
    bool torquescriptConvHooksRegistered = false;
    bool pickupActionRoundTrip = false;
    std::string conversationPriorityBranch;
    s32 guardHealthRemaining = 0;
    bool seqScrubPreviewValid = false;
    bool motionTimelineLoaded = false;
    float patrolMotionSampleX = 0.f;
    float combinedMountQuatYawDeg = 0.f;
    bool constraintSpecParsed = false;
    u32 alliesOutsideRadius = 0;
    bool radiusFilterPolicyValid = false;
    float effectiveAllyRadius = 0.f;
    bool parallelPolicyValid = false;
    u32 parallelSuccessThreshold = 0;
    u32 aggressiveBranchLineCount = 0;
    std::string aggressivePeekLine;
    u32 welcomeBranchLineCount = 0;
    bool examineActionRoundTrip = false;
    bool broadphaseDecoyRemoved = false;
    u32 broadphaseBodyCountAfterRemove = 0;
    bool codegenImportProfileOk = false;
    bool codegenReloadProfileOk = false;
    u32 briefingDispatchAllLines = 0;
    u32 conversationGrantCountSnapshot = 0;
    bool parallelPolicyHasGuards = false;
    u32 broadphaseRaycastStubHits = 0;
    bool castBeginOk = false;
    u32 cuePreviewMidTimelineCount = 0;
    bool leverUseStubExecuted = false;
    bool guardHealApplied = false;
    std::string useVerbFromActionKind;
    bool toolRoomDoorOpened = false;
    bool labSidearmPickupConsumed = false;
    bool leverPickupStubExecuted = false;
    bool aggressiveInventoryRequirementMet = false;
    bool aggressiveInventoryRequirementUnmetBeforePass = false;
    bool parallelPolicyTreeValid = false;
    u32 parallelFailThreshold = 0;
    bool allyRadiusFinite = false;
    bool pickupVerbFromActionKind = false;
    float mountEulerRoundTripYaw = 0.f;
    bool castReachedDeliveryPhase = false;
    bool guardNpcStateAdvanced = false;
    u32 codegenCsTreeNodeCount = 0;
    bool uaiskCsModuleParsed = false;
    float allyDistanceSqSample = 0.f;
    bool allyDistanceWithinRadius = false;
    u32 effectiveMinAllyCount = 0;
    float allyRadiusSqFromPolicy = 0.f;
    bool broadphaseProxyFiltersCollide = false;
    bool radioStoppedOnLever = false;
    bool leverExamineStubExecuted = false;
    bool elifVeteranBranchAllowed = false;
    u32 conversationLineDispatches = 0;
    bool toolDoorOpenMessageOk = false;
    u32 skeletalBoneIndexResolved = 0;
    u32 weaponMountAnimApplyCount = 0;
    bool shapeConstraintParsed = false;
    bool introTimelineAssetLoaded = false;
    u32 interactionQueueFailedCount = 0;
    bool interactionQueueClearedPending = false;
    float patrolMotionPathLength = 0.f;
    bool unlimitedAllyRadiusSentinel = false;
    float grantedWeaponDamage = 0.f;
    bool mechanicsBridgeVerticalSlice = false;
    bool registryResolvedInteractable = false;
    bool registryUseBlockedWithoutItem = false;
    bool missionImpactHookDispatched = false;
    bool missionSpellReadyDispatched = false;
    bool missionExecuteImpactViaBody = false;
    bool inventoryProviderInterfaceOk = false;
    bool eventTrackScriptHookOk = false;
    bool convHooksLoaderParsed = false;
    bool blackboardScalarRoundTrip = false;
    bool directHitscanOk = false;
    u32 specsLoaderNodeCount = 0;
    bool hudPromptViaPromptFor = false;
    bool convTsHooksLoaderParsed = false;
    bool nodeRegistryMoveTowardFactory = false;
    bool uaiskTemplateHooksNonZero = false;
    bool broadphaseProxyMaskCollideOk = false;
    bool blackboardViewBound = false;
    bool adventurePickupViaSystem = false;
    u32 missionMisHookParseCount = 0;
    bool missionScheduledPendingDelayed = false;
    bool hybridTimelineDriveSampled = false;
    u32 effectGraphCompletedCount = 0;
    u32 missilePipelineCompleted = 0;
    u32 treeInotifyReloadCount = 0;
    u32 treeInotifyPollCount = 0;
    u32 broadphaseMaskedOverlapHits = 0;
    bool nearestAllyFound = false;
    u32 nearestAllyIndex = 0;
    bool parallelPreconditionsMet = false;
    bool leverUseActionRoundTrip = false;
    float leverOpenPropertySample = 0.f;
    float leverOpenPropertyMidRampSample = 0.f;
    fuse::cinematics::MountEulerDeg combinedMountEuler{};
    float initialClearR = 0.f;
    float initialClearG = 0.f;
    float initialClearB = 0.f;
};

void setup(State& state, fuse::hybrid::HybridComposer& composer);
void tickFrame(State& state, fuse::hybrid::HybridComposer& composer, const fuse::frame::FrameCtx& ctx);

struct VerifyResult {
    bool ok = true;
    const char* message = nullptr;
};

VerifyResult verify(const State& state, fuse::hybrid::HybridComposer& composer);

} // namespace fuse::hybrid::gates
