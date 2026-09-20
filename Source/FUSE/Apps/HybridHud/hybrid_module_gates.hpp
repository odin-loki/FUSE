#pragma once

#include <fuse/adventure/animation_bind_pose_bridge.hpp>
#include <fuse/adventure/hud_prompt_interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/conversation_script_vm.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/outpost_spawn.hpp>
#include <fuse/adventure/skeletal_mount_stub.hpp>
#include <fuse/adventure/weapon_grant_pipeline.hpp>
#include <fuse/adventure/weapon_mount_animation.hpp>
#include <fuse/adventure/combat_hitscan_stub.hpp>
#include <fuse/adventure/weapon_combat_loop.hpp>
#include <fuse/adventure/weapon_runtime.hpp>
#include <fuse/ai/uaisk_cs_codegen.hpp>
#include <fuse/ai/uaisk_tree_reload.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/cinematics/timeline_host_stub.hpp>
#include <fuse/dimension/world_handle.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/fx/afx_mission_script_vm.hpp>
#include <fuse/fx/fx_composer.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/mechanics/broadphase_trigger_sync.hpp>
#include <fuse/mechanics/broadphase_world_stub.hpp>
#include <fuse/mechanics/bt_dbvt_bridge.hpp>
#include <fuse/mechanics/console_method_component.hpp>
#include <fuse/mechanics/counter_component.hpp>
#include <fuse/mechanics/delay_component.hpp>
#include <fuse/mechanics/interactable.hpp>
#include <fuse/mechanics/message_component.hpp>
#include <fuse/mechanics/animate_component.hpp>
#include <fuse/mechanics/health_component.hpp>
#include <fuse/mechanics/follow_component.hpp>
#include <fuse/mechanics/move_component.hpp>
#include <fuse/mechanics/radio_component.hpp>
#include <fuse/mechanics/path_component.hpp>
#include <fuse/mechanics/waypoint_component.hpp>
#include <fuse/mechanics/look_at_component.hpp>
#include <fuse/mechanics/physics_broadphase_bridge.hpp>
#include <fuse/mechanics/timer_component.hpp>
#include <fuse/mechanics/physics_trigger_bridge.hpp>
#include <fuse/mechanics/polyhedron_trigger.hpp>
#include <fuse/mechanics/registry.hpp>
#include <fuse/mechanics/rotate_component.hpp>
#include <fuse/mechanics/switch_component.hpp>
#include <fuse/mechanics/toggle_component.hpp>
#include <fuse/cinematics/cue_preview.hpp>
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

#ifndef FUSE_HYBRID_GATES_WAVE16
#define FUSE_HYBRID_GATES_WAVE16 1
#endif

#ifndef FUSE_HYBRID_GATES_WAVE17
#define FUSE_HYBRID_GATES_WAVE17 1
#endif

#ifndef FUSE_HYBRID_GATES_WAVE18
#define FUSE_HYBRID_GATES_WAVE18 1
#endif

#ifndef FUSE_HYBRID_GATES_WAVE19
#define FUSE_HYBRID_GATES_WAVE19 1
#endif

#ifndef FUSE_HYBRID_GATES_WAVE20
#define FUSE_HYBRID_GATES_WAVE20 1
#endif

#ifndef FUSE_HYBRID_GATES_WAVE21
#define FUSE_HYBRID_GATES_WAVE21 1
#endif

#ifndef FUSE_HYBRID_GATES_WAVE22
#define FUSE_HYBRID_GATES_WAVE22 1
#endif

namespace fuse::hybrid::gates {

constexpr int kFrameCount = 60;
constexpr float kDt = 1.f / 60.f;
constexpr fuse::cinematics::TimelineMs kTimelineDurationMs = 1'000;

/// Prestarter §10 U5 module gate wiring for demo_hybrid_hud + headless tests.
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

    fuse::mechanics::MechanicsRegistry mechanicsRegistry;
    fuse::mechanics::InteractableComponent leverInteractable{"lever_interactable"};
    fuse::mechanics::ToggleComponent leverToggle{"lever_toggle"};
    fuse::mechanics::SwitchComponent leverSwitch{"lever_switch", false, "armed", "safe"};
    fuse::mechanics::ConsoleMethodComponent leverConsole{"lever_console"};
    fuse::mechanics::DelayComponent leverDelay{"lever_delay", 100};
    fuse::mechanics::RotateComponent leverRotate{"lever_rotate", 45.f};
    fuse::mechanics::CounterComponent leverCounter{"lever_counter", 0, 1};
    fuse::mechanics::MessageComponent leverMessage{"lever_message", "Lever activated"};
    fuse::mechanics::PolyhedronTriggerZone leverTrigger;
    fuse::mechanics::PhysicsTriggerBridge physicsTriggerBridge;
    fuse::mechanics::BroadphaseTriggerSync broadphaseTriggerSync;
    fuse::mechanics::PhysicsBroadphaseBridge physicsBroadphaseBridge;
    fuse::mechanics::PathComponent patrolPath{"outpost_patrol"};
    fuse::mechanics::TimerComponent patrolTimer{"outpost_patrol_timer", 0.25f};
    fuse::mechanics::AnimateComponent leverAnimate{"lever_animate", 0.5f};
    fuse::mechanics::WaypointComponent patrolWaypoint{"outpost_patrol_wp", 4.f, 0.f, 0.f};
    fuse::mechanics::LookAtComponent guardLookAt{"guard_look_at", 120.f};
    fuse::mechanics::MoveComponent allyMove{"ally_move", 0.8f};
    fuse::mechanics::FollowComponent allyFollow{"ally_follow", 0.6f};
    fuse::mechanics::RadioComponent leverRadio{"lever_radio", "outpost_alert"};
    fuse::mechanics::HealthComponent guardHealth{"outpost_guard_health", 100};
    fuse::mechanics::BroadphaseWorldStub broadphaseWorld;
    fuse::mechanics::BtDbvtBridge btDbvtBridge;

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
    fuse::adventure::ConversationScriptVm conversationScriptVm;
    fuse::adventure::HudPromptInteractable hudPrompt{"Press E to activate lever"};
    fuse::adventure::OutpostStubContent outpostContent;
    fuse::adventure::OutpostSpawnBundle outpostSpawn;

    fuse::cinematics::VActorBridge vactorBridge;
    fuse::cinematics::TimelineHostStub timelineHostStub;
    fuse::cinematics::TimelineMs lastTimelineMs = 0;

    std::string hudPromptText;
    std::string guardLineText;
    bool agentInsideTrigger = false;
    bool loadedOutpostStub = false;
    bool spawnedOutpostInteractables = false;
    bool appliedOutpostPlacements = false;
    bool weaponGranted = false;
    u32 cuePreviewCount = 0;
    u32 broadphaseRaycastHits = 0;
    u32 broadphaseDbvtHits = 0;
    bool weaponFired = false;
    bool combatLoopFired = false;
    bool weaponReloaded = false;
    bool timelineHostWired = false;
    u32 nestedCompositeNodeCount = 0;
    bool booleanExpressionValid = false;
    u32 btDbvtOverlapHits = 0;
    u32 btDbvtAabbHits = 0;
    u32 delayedMissionDispatchCount = 0;
    u32 missionCallCount = 0;
    u32 missionScheduleCount = 0;
    u32 timelineHostAdvanceCount = 0;
    u32 conversationInjectCount = 0;
    u32 conversationStateChainSteps = 0;
    u32 hitscanPelletHits = 0;
    u32 hitscanPenetrationLayers = 0;
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

VerifyResult verify(const State& state, const fuse::hybrid::HybridComposer& composer);

} // namespace fuse::hybrid::gates
