#include <fuse/adventure/conversation_interactable.hpp>
#include <fuse/adventure/conversation_script_loader.hpp>
#include <fuse/adventure/skeletal_mount_stub.hpp>
#include <fuse/adventure/weapon_mount_animation.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/weapon_grant_pipeline.hpp>
#include <fuse/adventure/weapon_pickup_interactable.hpp>
#include <fuse/adventure/weapon_runtime.hpp>
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

} // namespace

int main() {
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

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure_weapon_conversation: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure_weapon_conversation: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
