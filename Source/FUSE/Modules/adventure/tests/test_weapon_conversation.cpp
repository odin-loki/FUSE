#include <fuse/adventure/combat_hitscan_stub.hpp>
#include <fuse/adventure/friendly_fire.hpp>
#include <fuse/adventure/conversation_interactable.hpp>
#include <fuse/adventure/conversation_script_vm.hpp>
#include <fuse/adventure/conversation_script_loader.hpp>
#include <fuse/adventure/skeletal_mount_stub.hpp>
#include <fuse/adventure/animation_bind_pose_bridge.hpp>
#include <fuse/adventure/weapon_mount_animation.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/line_count.hpp>
#include <fuse/adventure/weapon_grant_pipeline.hpp>
#include <fuse/adventure/weapon_pickup_interactable.hpp>
#include <fuse/adventure/weapon_combat_loop.hpp>
#include <fuse/adventure/weapon_runtime.hpp>
#include <fuse/mechanics/health_component.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/core/init.hpp>

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

void testLineCountOk() {
    expectTrue(fuse::adventure::line_count_ok(1), "line_count_ok(1)");
    expectTrue(fuse::adventure::line_count_ok(256), "line_count_ok(256)");
    expectTrue(!fuse::adventure::line_count_ok(0), "line_count_ok(0)");
    expectTrue(!fuse::adventure::line_count_ok(257), "line_count_ok(257)");
}

void testFriendlyHitAllowed() {
    expectTrue(fuse::adventure::friendly_hit_allowed(1u, 2u, false),
               "friendly_hit_allowed(1, 2, false)");
    expectTrue(!fuse::adventure::friendly_hit_allowed(1u, 1u, false),
               "friendly_hit_allowed(1, 1, false)");
    expectTrue(fuse::adventure::friendly_hit_allowed(1u, 1u, true),
               "friendly_hit_allowed(1, 1, true)");
}

} // namespace

int main() {
    testLineCountOk();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }

    testFriendlyHitAllowed();

    fuse::core::initialize();

    fuse::adventure::Inventory inventory;
    inventory.setMaxLimit(fuse::adventure::ItemId("plasma_rifle"), 1);
    inventory.setMaxLimit(fuse::adventure::ItemId("energy_cell"), 99);
    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inventory;
    ctx.actorName = "player";

    fuse::adventure::WeaponPickupInteractable rifle(fuse::adventure::ItemId("plasma_rifle"),
                                                     fuse::adventure::ItemId("energy_cell"),
                                                     20);
    const fuse::adventure::InteractResult pickup =
        rifle.onPickup(ctx, fuse::adventure::ItemId("plasma_rifle"), 1);
    expectTrue(pickup == fuse::adventure::InteractResult::PickedUp, "weapon pickup succeeds");
    expectTrue(inventory.hasInventory(fuse::adventure::ItemId("plasma_rifle")), "weapon granted");
    expectTrue(inventory.hasInventory(fuse::adventure::ItemId("energy_cell")), "ammo granted");
    expectTrue(inventory.activeWeapon().name == "plasma_rifle", "weapon equipped on pickup");
    expectTrue(rifle.consumed(), "weapon pickup consumed");

    fuse::adventure::WeaponPickupInteractable pistol(fuse::adventure::ItemId("sidearm"),
                                                     fuse::adventure::ItemId("energy_cell"), 12);
    fuse::adventure::WeaponGrantPipeline grantPipeline;
    fuse::adventure::WeaponGrantRequest grantRequest{};
    grantRequest.weapon = fuse::adventure::ItemId("sidearm");
    grantRequest.ammo = fuse::adventure::ItemId("energy_cell");
    grantRequest.ammoAmount = 12;
    grantRequest.stats.damage = 8.f;
    fuse::adventure::WeaponRuntime grantedRuntime;
    expectTrue(grantPipeline.grantOnPickup(ctx, pistol, grantRequest, grantedRuntime),
               "weapon grant pipeline grants pickup");
    expectTrue(grantPipeline.grantCount() == 1u, "weapon grant pipeline counted");
    expectTrue(grantedRuntime.stats().damage == 8.f, "weapon grant pipeline wires stats");

    fuse::adventure::WeaponRuntime weaponRuntime;
    weaponRuntime.setAmmoType(fuse::adventure::ItemId("energy_cell"));
    fuse::adventure::WeaponStats stats{};
    stats.damage = 25.f;
    stats.range = 80.f;
    weaponRuntime.setStats(stats);
    expectTrue(weaponRuntime.fire(inventory), "weapon runtime fires with ammo");
    expectTrue(weaponRuntime.fireCount() == 1u, "weapon runtime fire counted");
    expectTrue(weaponRuntime.lastDamageDealt() == 25.f, "weapon runtime records damage stat");
    expectTrue(inventory.getInventory(fuse::adventure::ItemId("energy_cell")) == 31u,
               "weapon runtime consumes ammo after grant pipeline");

    fuse::adventure::ConversationBranch polite;
    polite.id = "polite";
    polite.lines = {"Thank you, traveler. Proceed with caution."};
    fuse::adventure::ConversationInteractable guard(
        {"Halt. State your business.", "The reactor is unstable — keep moving."}, {polite});
    fuse::adventure::InteractionSystem system;
    const std::string line0 = system.converse(ctx, guard);
    expectTrue(line0 == "The reactor is unstable — keep moving.", "conversation advances line");
    expectTrue(guard.converseCount() == 1u, "converse count tracked");

    const std::string branchLine = system.converseBranch(ctx, guard, "polite");
    expectTrue(branchLine == "Thank you, traveler. Proceed with caution.", "conversation branch selected");
    expectTrue(guard.activeBranchId() == "polite", "active branch tracked");

    fuse::adventure::ConversationScriptVm scriptVm;
    fuse::adventure::registerOutpostConversationScriptHooks(scriptVm);
    expectTrue(!scriptVm.canDispatchBranch("outpost_guard", "aggressive", ctx),
               "aggressive branch gated without security pass");
    inventory.setMaxLimit(fuse::adventure::ItemId("security_pass"), 1);
    inventory.incInventory(fuse::adventure::ItemId("security_pass"), 1);
    expectTrue(scriptVm.canDispatchBranch("outpost_guard", "aggressive", ctx),
               "aggressive branch allowed with security pass");
    expectTrue(scriptVm.branchLineCount("outpost_guard", "aggressive") == 2u,
               "conversation VM tracks multi-line branch");
    expectTrue(scriptVm.peekBranchLine("outpost_guard", "aggressive", 1) ==
                   "You may pass — this time.",
               "conversation VM peeks branch line");
    fuse::adventure::ConversationBranch aggressiveBranch;
    aggressiveBranch.id = "aggressive";
    aggressiveBranch.lines = {"Stand down."};
    inventory.setMaxLimit(fuse::adventure::ItemId("security_badge"), 1);
    fuse::adventure::ConversationInteractable guardAggressive({"Halt."}, {aggressiveBranch});
    scriptVm.dispatchBranch("outpost_guard", "aggressive", ctx, guardAggressive);
    expectTrue(scriptVm.grantCount() == 1u, "conversation VM grants item on branch");
    expectTrue(inventory.hasInventory(fuse::adventure::ItemId("security_badge")),
               "conversation VM grant item in inventory");

    static const char* kConvText =
        "# outpost guard conv\n"
        "branch outpost_guard veteran\n"
        "line Welcome back, veteran.\n"
        "requires service_medal 1\n";
    fuse::adventure::ConversationScriptVm loadedVm;
    expectTrue(fuse::adventure::register_conversation_hooks_from_text(kConvText, loadedVm),
               "conversation script loader registers hooks");
    inventory.setMaxLimit(fuse::adventure::ItemId("service_medal"), 1);
    inventory.incInventory(fuse::adventure::ItemId("service_medal"), 1);
    fuse::adventure::ConversationBranch veteranBranch;
    veteranBranch.id = "veteran";
    veteranBranch.lines = {"Welcome back, veteran."};
    fuse::adventure::ConversationInteractable guardVeteran({"Halt."}, {veteranBranch});
    expectTrue(loadedVm.dispatchAllLines("outpost_guard", "veteran", ctx, guardVeteran) == 1u,
               "conversation VM dispatches all branch lines");
    expectTrue(loadedVm.lineDispatchCount() == 1u, "conversation VM line dispatch counted");

    fuse::adventure::WeaponMountAnimationStub mountAnim;
    fuse::adventure::WeaponMountPose pose{};
    pose.mountYawDeg = 15.f;
    pose.mountPitchDeg = -5.f;
    mountAnim.setPose(pose);
    expectTrue(mountAnim.applyOnGrant(inventory, fuse::adventure::ItemId("sidearm")),
               "weapon mount animation applies on grant");
    expectTrue(mountAnim.applyCount() == 1u, "weapon mount animation counted");

    fuse::adventure::SkeletalMountStub skeletalMount;
    fuse::adventure::SkeletalBoneMount bone{};
    bone.boneName = "spine_weapon";
    bone.yawDeg = 20.f;
    bone.pitchDeg = -8.f;
    skeletalMount.setBoneMount(bone);
    fuse::adventure::WeaponMountAnimationStub skeletalAnim;
    expectTrue(skeletalMount.applyToMountAnimation(skeletalAnim), "skeletal mount applies to animation stub");
    expectTrue(skeletalMount.applyCount() == 1u, "skeletal mount apply counted");
    expectTrue(skeletalAnim.pose().mountPoint == "spine_weapon", "skeletal mount bone name wired");
    expectTrue(skeletalMount.boneMount().boneIndex == 8u, "skeletal mount resolves bone index");

    static const char* kPriorityConvText =
        "branch outpost_guard polite\n"
        "line Hello.\n"
        "priority 1\n"
        "branch outpost_guard urgent\n"
        "line Move along.\n"
        "priority 5\n";
    fuse::adventure::ConversationScriptVm priorityVm;
    expectTrue(fuse::adventure::register_conversation_hooks_from_text(kPriorityConvText, priorityVm),
               "priority conversation hooks loaded");
    std::string chosenBranch;
    expectTrue(priorityVm.chooseHighestPriorityBranch("outpost_guard", ctx, chosenBranch),
               "conversation VM chooses highest priority branch");
    expectTrue(chosenBranch == "urgent", "urgent branch wins by priority");

    fuse::adventure::ConversationBranch multiLineBranch;
    multiLineBranch.id = "multi";
    multiLineBranch.lines = {"Line one.", "Line two."};
    fuse::adventure::ConversationInteractable guardMulti({"Halt."}, {multiLineBranch});
    fuse::adventure::ConversationScriptHook multiHook;
    multiHook.npcId = "outpost_guard";
    multiHook.branchId = "multi";
    multiHook.lines = {"Line one.", "Line two."};
    scriptVm.registerHook(multiHook);
    expectTrue(scriptVm.dispatchAllLines("outpost_guard", "multi", ctx, guardMulti) == 2u,
               "conversation VM dispatches all lines");
    expectTrue(scriptVm.lastLineDispatched() == "Line two.",
               "conversation VM records last dispatched line");

    static const char* kTsConvText =
        "function onConversation_outpost_guard_welcome() {\n"
        "  echo(\"Welcome to the outpost.\");\n"
        "}\n";
    fuse::adventure::ConversationScriptVm tsVm;
    expectTrue(fuse::adventure::register_conversation_hooks_from_torquescript(kTsConvText, tsVm),
               "TorqueScript conversation loader registers hooks");
    expectTrue(tsVm.branchLineCount("outpost_guard", "welcome") == 1u,
               "TorqueScript conversation branch line parsed");

    static const char* kElifConvText =
        "branch outpost_guard veteran\n"
        "requires service_medal 1\n"
        "elif requires security_badge 1\n"
        "line Alternate clearance accepted.\n";
    fuse::adventure::ConversationScriptVm elifVm;
    expectTrue(fuse::adventure::register_conversation_hooks_from_text(kElifConvText, elifVm),
               "elif inventory branch loaded");
    inventory.decInventory(fuse::adventure::ItemId("service_medal"), 1);
    inventory.incInventory(fuse::adventure::ItemId("security_badge"), 1);
    expectTrue(elifVm.canDispatchBranch("outpost_guard", "veteran", ctx),
               "elif inventory branch allowed via alternate item");

    fuse::animation::Skeleton bindSkeleton;
    bindSkeleton.bone_count = 16;
    bindSkeleton.bones.resize(16);
    fuse::animation::PoseSoA bindPose = fuse::animation::PoseSoA::from_bind_pose(bindSkeleton);
    fuse::adventure::AnimationBindPoseBridge bindBridge;
    fuse::adventure::SkeletalMountStub bindSkeletalMount;
    fuse::adventure::SkeletalBoneMount bindBone{};
    bindBone.boneName = "weapon_shoulder";
    bindBone.offsetX = 0.1f;
    bindSkeletalMount.setBoneMount(bindBone);
    fuse::adventure::WeaponMountAnimationStub bindMountAnim;
    fuse::adventure::WeaponMountPose bindMountPose{};
    bindMountPose.mountPoint = "weapon_shoulder";
    bindMountPose.shoulderOffset = 0.05f;
    bindMountAnim.setPose(bindMountPose);
    expectTrue(bindBridge.applyMountToBindPose(bindSkeletalMount, bindMountAnim, bindSkeleton, bindPose),
               "animation bind pose bridge applies mount offset");
    expectTrue(bindBridge.applyCount() == 1u, "animation bind pose bridge counted");
    expectTrue(bindBridge.lastBoneIndex() == 12u, "animation bind pose bridge resolves bone index");

    inventory.setMaxLimit(fuse::adventure::ItemId("scatter_gun"), 1);
    fuse::adventure::WeaponPickupInteractable mountRifle(fuse::adventure::ItemId("scatter_gun"),
                                                         fuse::adventure::ItemId("energy_cell"), 10);
    fuse::adventure::WeaponGrantPipeline mountPipeline;
    fuse::adventure::WeaponGrantRequest mountRequest{};
    mountRequest.weapon = fuse::adventure::ItemId("scatter_gun");
    mountRequest.ammo = fuse::adventure::ItemId("energy_cell");
    mountRequest.ammoAmount = 10;
    fuse::adventure::WeaponRuntime mountRuntime;
    fuse::adventure::WeaponMountAnimationStub mountAnimStub;
    fuse::adventure::SkeletalMountStub mountSkeletal;
    fuse::adventure::SkeletalBoneMount mountBone{};
    mountBone.boneName = "weapon_shoulder";
    mountSkeletal.setBoneMount(mountBone);
    expectTrue(mountPipeline.grantOnPickupWithMount(ctx, mountRifle, mountRequest, mountRuntime,
                                                      mountAnimStub, mountSkeletal),
               "weapon grant pipeline with mount chain");
    expectTrue(mountPipeline.mountGrantCount() == 1u, "weapon mount grant counted");

    fuse::adventure::ConversationScriptVm bestVm;
    fuse::adventure::registerOutpostConversationScriptHooks(bestVm);
    fuse::adventure::ConversationBranch armedBranch;
    armedBranch.id = "armed";
    armedBranch.lines = {"I see you are armed. Keep that rifle stowed."};
    fuse::adventure::ConversationInteractable guardArmed({"Halt."}, {armedBranch});
    expectTrue(bestVm.dispatchBestBranch("outpost_guard", ctx, guardArmed),
               "conversation VM dispatchBestBranch chooses armed");
    expectTrue(bestVm.lastBranchDispatched() == "armed", "armed branch wins by priority");
    expectTrue(bestVm.peekBestBranchLine("outpost_guard", ctx) ==
                   "I see you are armed. Keep that rifle stowed.",
               "conversation VM peekBestBranchLine armed");
    expectTrue(bestVm.dispatchBestBranchAllLines("outpost_guard", ctx, guardArmed) >= 1u,
               "conversation VM dispatchBestBranchAllLines");

    fuse::adventure::ConversationBranch injectBranch;
    injectBranch.id = "inject";
    injectBranch.lines = {"Injected line one.", "Injected line two."};
    fuse::adventure::ConversationInteractable guardInject({"Halt."}, {injectBranch});
    fuse::adventure::ConversationScriptHook injectHook;
    injectHook.npcId = "outpost_guard";
    injectHook.branchId = "inject";
    injectHook.lines = {"Injected line one.", "Injected line two."};
    scriptVm.registerHook(injectHook);
    expectTrue(scriptVm.dispatchAllLines("outpost_guard", "inject", ctx, guardInject) == 2u,
               "conversation VM injects script lines");
    expectTrue(guardInject.injectedLineCount() == 2u, "conversation interactable injected line count");

    fuse::mechanics::HealthComponent targetHealth("target_dummy", 100);
    fuse::adventure::WeaponCombatLoop combatLoop;
    fuse::adventure::CombatHitscanStub hitscanLoop;
    combatLoop.setActiveWeapon(&weaponRuntime);
    combatLoop.setTarget(&targetHealth);
    combatLoop.setHitscanStub(&hitscanLoop);
    combatLoop.setTargetPosition(10.f, 0.f, 0.f);
    combatLoop.tick(0.f);
    expectTrue(combatLoop.tryFire(inventory), "weapon combat loop fires");
    expectTrue(combatLoop.hitscanHitCount() >= 1u, "weapon combat loop hitscan hit counted");
    expectTrue(combatLoop.damageApplyCount() == 1u, "weapon combat loop applies damage");
    expectTrue(targetHealth.currentHealth() < 100, "weapon combat loop reduced target health");
    expectTrue(!combatLoop.tryFire(inventory), "weapon combat loop respects fire-rate gate");
    combatLoop.tick(1.f);
    expectTrue(combatLoop.tryFire(inventory), "weapon combat loop fires after cooldown");

    fuse::adventure::WeaponRuntime reloadRuntime;
    reloadRuntime.setAmmoType(fuse::adventure::ItemId("energy_cell"));
    fuse::adventure::WeaponStats reloadStats{};
    reloadStats.magazineSize = 2;
    reloadStats.reloadMs = 100;
    reloadRuntime.setStats(reloadStats);
    reloadRuntime.fire(inventory);
    reloadRuntime.fire(inventory);
    expectTrue(reloadRuntime.reload(inventory), "weapon runtime begins reload");
    expectTrue(reloadRuntime.advanceReload(120), "weapon runtime completes reload");
    expectTrue(reloadRuntime.magazineAmmo() == 2u, "weapon runtime refills magazine");

    fuse::adventure::CombatHitscanStub hitscan;
    hitscan.setSpreadDeg(5.f);
    fuse::adventure::WeaponStats hitscanStats{};
    hitscanStats.damage = 25.f;
    hitscanStats.range = 80.f;
    const fuse::adventure::HitscanResult hit =
        hitscan.fireHitscan(hitscanStats, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 10.f, 0.f, 0.f);
    expectTrue(hit.hit, "hitscan stub hits in-range target");
    expectTrue(hit.damage == 25.f, "hitscan stub applies weapon damage");

    fuse::adventure::WeaponStats falloffStats{};
    falloffStats.damage = 40.f;
    falloffStats.range = 80.f;
    falloffStats.falloffMinScale = 0.f;
    fuse::adventure::CombatHitscanStub falloffHitscan;
    const fuse::adventure::HitscanResult nearFalloff =
        falloffHitscan.fireHitscan(falloffStats, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 20.f, 0.f, 0.f);
    const fuse::adventure::HitscanResult farFalloff =
        falloffHitscan.fireHitscan(falloffStats, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 60.f, 0.f, 0.f);
    expectTrue(nearFalloff.hit && nearFalloff.damage == 30.f, "hitscan falloff keeps higher damage up close");
    expectTrue(farFalloff.hit && farFalloff.damage == 10.f, "hitscan falloff reduces damage at longer range");

    fuse::adventure::WeaponStats flatStats{};
    flatStats.damage = 40.f;
    flatStats.range = 80.f;
    const fuse::adventure::HitscanResult flatHit =
        falloffHitscan.fireHitscan(flatStats, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 60.f, 0.f, 0.f);
    expectTrue(flatHit.hit && flatHit.damage == 40.f, "hitscan default falloff scale deals full damage");

    fuse::adventure::WeaponStats minScaleStats{};
    minScaleStats.damage = 40.f;
    minScaleStats.range = 80.f;
    minScaleStats.falloffMinScale = 0.5f;
    const fuse::adventure::HitscanResult edgeFalloff =
        falloffHitscan.fireHitscan(minScaleStats, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 80.f, 0.f, 0.f);
    expectTrue(edgeFalloff.hit && edgeFalloff.damage == 20.f, "hitscan falloff reaches min scale at max range");

    const fuse::adventure::HitscanResult beyondFalloff =
        falloffHitscan.fireHitscan(falloffStats, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 100.f, 0.f, 0.f);
    expectTrue(!beyondFalloff.hit && beyondFalloff.damage == 0.f, "hitscan falloff does not damage beyond range");

    fuse::adventure::Inventory falloffBag;
    falloffBag.setMaxLimit(fuse::adventure::ItemId("energy_cell"), 30);
    falloffBag.incInventory(fuse::adventure::ItemId("energy_cell"), 10);
    falloffBag.setActiveWeapon(fuse::adventure::ItemId("plasma_rifle"));
    fuse::adventure::WeaponRuntime falloffRuntime;
    falloffRuntime.setAmmoType(fuse::adventure::ItemId("energy_cell"));
    falloffRuntime.setStats(falloffStats);
    fuse::mechanics::HealthComponent falloffHealth("falloff_dummy", 100);
    fuse::adventure::WeaponCombatLoop falloffLoop;
    falloffLoop.setActiveWeapon(&falloffRuntime);
    falloffLoop.setTarget(&falloffHealth);
    falloffLoop.setHitscanStub(&falloffHitscan);
    falloffLoop.setTargetPosition(40.f, 0.f, 0.f);
    expectTrue(falloffLoop.tryFire(falloffBag), "weapon combat loop fires with falloff");
    expectTrue(falloffRuntime.lastDamageDealt() == 40.f, "weapon runtime keeps unscaled damage stat");
    expectTrue(falloffLoop.lastAppliedDamage() == 20.f, "weapon combat loop applies hitscan falloff damage");
    expectTrue(falloffHealth.currentHealth() == 80, "hitscan falloff reduces health by scaled damage");

    fuse::adventure::ConversationScriptVm stateVm;
    fuse::adventure::registerOutpostConversationScriptHooks(stateVm);
    fuse::adventure::ConversationBranch politeStateBranch;
    politeStateBranch.id = "polite";
    politeStateBranch.lines = {"Thank you, traveler. Proceed with caution."};
    fuse::adventure::ConversationInteractable guardPolite({"Halt."}, {politeStateBranch});
    expectTrue(stateVm.advanceNpcState("outpost_guard", ctx, guardPolite),
               "conversation state machine advances greeting");
    expectTrue(stateVm.npcState("outpost_guard") == fuse::adventure::ConversationState::Greeting,
               "conversation state machine records greeting");
    expectTrue(stateVm.stateAdvanceCount() >= 1u, "conversation state advance counted");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure_weapon_conversation: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure_weapon_conversation: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
