#include <fuse/animation/animator.hpp>
#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/clip.hpp>
#include <fuse/animation/ik_solver.hpp>
#include <fuse/animation/retarget.hpp>
#include <fuse/animation/skinning.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (expected %.4f, got %.4f)\n", message, expected, actual);
        ++g_failures;
    }
}

fuse::animation::Skeleton makeTwoBoneSkeleton() {
    fuse::animation::Skeleton skel;
    fuse::animation::Bone root{};
    std::strncpy(root.name, "root", sizeof(root.name) - 1);
    root.parent_index = -1;
    root.local_transform = fuse::animation::mat4::identity();

    fuse::animation::Bone child{};
    std::strncpy(child.name, "child", sizeof(child.name) - 1);
    child.parent_index = 0;
    child.local_transform = fuse::animation::mat4::identity();
    child.local_transform.data[13] = 1.f;

    skel.bones = {root, child};
    skel.bone_count = 2;
    return skel;
}

fuse::animation::Skeleton makeThreeBoneSkeleton() {
    fuse::animation::Skeleton skel = makeTwoBoneSkeleton();

    fuse::animation::Bone grandchild{};
    std::strncpy(grandchild.name, "grandchild", sizeof(grandchild.name) - 1);
    grandchild.parent_index = 1;
    grandchild.local_transform = fuse::animation::mat4::identity();
    grandchild.local_transform.data[13] = 1.f;

    skel.bones.push_back(grandchild);
    skel.bone_count = 3;
    return skel;
}

fuse::animation::Skeleton makeLimbSkeleton() {
    fuse::animation::Skeleton skel;

    fuse::animation::Bone root{};
    std::strncpy(root.name, "hip", sizeof(root.name) - 1);
    root.parent_index = -1;
    root.local_transform = fuse::animation::mat4::identity();

    fuse::animation::Bone knee{};
    std::strncpy(knee.name, "knee", sizeof(knee.name) - 1);
    knee.parent_index = 0;
    knee.local_transform = fuse::animation::mat4::identity();
    knee.local_transform.data[13] = 1.f;

    fuse::animation::Bone ankle{};
    std::strncpy(ankle.name, "ankle", sizeof(ankle.name) - 1);
    ankle.parent_index = 1;
    ankle.local_transform = fuse::animation::mat4::identity();
    ankle.local_transform.data[13] = 1.f;

    skel.bones = {root, knee, ankle};
    skel.bone_count = 3;
    return skel;
}

fuse::animation::Skeleton makeRetargetTargetSkeleton() {
    fuse::animation::Skeleton skel = makeTwoBoneSkeleton();

    fuse::animation::Bone prop{};
    std::strncpy(prop.name, "prop", sizeof(prop.name) - 1);
    prop.parent_index = 0;
    prop.local_transform = fuse::animation::mat4::identity();
    prop.local_transform.data[12] = 2.f;

    skel.bones.push_back(prop);
    skel.bone_count = 3;
    return skel;
}

fuse::animation::AnimationClip makePositionClip(fuse::u32 boneIndex, fuse::f32 localY) {
    fuse::animation::AnimationClip clip{};
    clip.duration = 1.f;
    fuse::animation::AnimationClip::BoneChannels channel{};
    channel.bone_index = boneIndex;
    channel.position.values_vec3 = {{0.f, localY, 0.f, 0.f}};
    clip.bone_channels.push_back(channel);
    return clip;
}

void testSkeletonHierarchy() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    expectTrue(skel.find_bone("root") == 0, "root bone lookup");
    expectTrue(skel.find_bone("child") == 1, "child bone lookup");

    const fuse::animation::Pose bindPose = fuse::animation::Pose::make_bind_pose(skel);
    expectTrue(bindPose.bone_count == 2u, "bind pose bone count");
    expectNear(bindPose.bone_world_transforms[1].data[13], 1.f, 1e-4f, "child inherits parent offset");
}

void testClipSampling() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip clip{};
    std::strncpy(clip.name, "lift", sizeof(clip.name) - 1);
    clip.duration = 1.f;

    fuse::animation::AnimationClip::BoneChannels channels{};
    channels.bone_index = 1;
    channels.position.times = {0.f, 1.f};
    channels.position.values_vec3 = {{0.f, 1.f, 0.f, 0.f}, {0.f, 2.f, 0.f, 0.f}};
    clip.bone_channels.push_back(channels);

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    clip.sample(0.5f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 2.5f, 0.05f, "clip samples position channel");
}

void testBlendNodeInterpolation() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();

    fuse::animation::AnimationClip clipA{};
    clipA.duration = 1.f;
    fuse::animation::AnimationClip::BoneChannels channelA{};
    channelA.bone_index = 1;
    channelA.position.times = {0.f, 1.f};
    channelA.position.values_vec3 = {{0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}};
    clipA.bone_channels.push_back(channelA);

    fuse::animation::AnimationClip clipB{};
    clipB.duration = 1.f;
    fuse::animation::AnimationClip::BoneChannels channelB{};
    channelB.bone_index = 1;
    channelB.position.times = {0.f, 1.f};
    channelB.position.values_vec3 = {{0.f, 3.f, 0.f, 0.f}, {0.f, 3.f, 0.f, 0.f}};
    clipB.bone_channels.push_back(channelB);

    auto nodeA = std::make_unique<fuse::animation::ClipNode>();
    nodeA->clip = &clipA;
    auto nodeB = std::make_unique<fuse::animation::ClipNode>();
    nodeB->clip = &clipB;

    fuse::f32 blend = 0.5f;
    fuse::animation::BlendNode2 blendNode;
    blendNode.a = std::move(nodeA);
    blendNode.b = std::move(nodeB);
    blendNode.blend_param = &blend;

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    blendNode.evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 3.f, 0.05f, "blend node lerps child translation");
}

void testBlendSpace1DParameterSample() {
    fuse::animation::BlendSpace1D space;
    space.entries.push_back({0.f, nullptr});
    space.entries.push_back({1.f, nullptr});
    space.entries.push_back({2.f, nullptr});

    const fuse::animation::BlendSpace1DSample sample = fuse::animation::sample_blend_space_1d(space, 0.5f);
    expectTrue(sample.lower_index == 0u, "blend space 1d sample lower bracket");
    expectTrue(sample.upper_index == 1u, "blend space 1d sample upper bracket");
    expectNear(sample.alpha, 0.5f, 1e-4f, "blend space 1d sample alpha");
}

void testBlendSpace2D() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();

    fuse::animation::AnimationClip clipA = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip clipB = makePositionClip(1, 4.f);

    fuse::animation::BlendSpace2D space;
    auto nodeA = std::make_unique<fuse::animation::ClipNode>();
    nodeA->clip = &clipA;
    auto nodeB = std::make_unique<fuse::animation::ClipNode>();
    nodeB->clip = &clipB;
    space.entries.push_back({{0.f, 0.f}, std::move(nodeA)});
    space.entries.push_back({{1.f, 0.f}, std::move(nodeB)});

    fuse::animation::vec2 velocity{0.5f, 0.f};
    space.param = &velocity;

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    space.evaluate_soa(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 3.5f, 0.1f, "blend space 2d evaluates into pose soa");
}

void testBlendSpace2DParameterSample() {
    fuse::animation::BlendSpace2D space;
    space.entries.push_back({{0.f, 0.f}, nullptr});
    space.entries.push_back({{1.f, 0.f}, nullptr});

    const fuse::animation::BlendSpace2DSample sample =
        fuse::animation::sample_blend_space_2d(space, {0.2f, 0.f});
    expectTrue(sample.weights.size() == 2u, "blend space 2d sample weight count");
    expectNear(sample.weights[0] + sample.weights[1], 1.f, 1e-4f, "blend space 2d sample weights normalize");
    expectTrue(sample.weights[0] > sample.weights[1], "blend space 2d sample favors nearer entry");
}

void testAdditiveBlendLayer() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip baseClip = makePositionClip(1, 2.f);
    fuse::animation::AnimationClip layerClip = makePositionClip(1, 5.f);

    auto baseNode = std::make_unique<fuse::animation::ClipNode>();
    baseNode->clip = &baseClip;
    auto layerNode = std::make_unique<fuse::animation::ClipNode>();
    layerNode->clip = &layerClip;

    fuse::animation::AdditiveBlendNode additive;
    additive.base = std::move(baseNode);
    additive.layer = std::move(layerNode);
    additive.masked_bones = {1};
    additive.layer_weight = 0.5f;

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    additive.evaluate_soa(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 5.5f, 0.05f, "additive blend applies half delta over base");
}

void testBlendTreeEvaluateSoA() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip clipA = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip clipB = makePositionClip(1, 3.f);

    auto nodeA = std::make_unique<fuse::animation::ClipNode>();
    nodeA->clip = &clipA;
    auto nodeB = std::make_unique<fuse::animation::ClipNode>();
    nodeB->clip = &clipB;

    fuse::f32 blend = 0.25f;
    fuse::animation::BlendNode2 blendNode;
    blendNode.a = std::move(nodeA);
    blendNode.b = std::move(nodeB);
    blendNode.blend_param = &blend;

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    blendNode.evaluate_soa(0.f, skel, pose);
    expectNear(pose.local_positions[1].y, 2.5f, 1e-4f, "blend tree evaluate_soa lerps local position");
}

void testStateMachineEnterExit() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();

    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip run = makePositionClip(1, 4.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    machine.add_state("idle", std::move(idleNode));
    machine.add_state("run", std::move(runNode));
    machine.states[0].on_exit = []() {};
    machine.states[1].on_enter = []() {};

    int exitCount = 0;
    int enterCount = 0;
    machine.states[0].on_exit = [&]() { ++exitCount; };
    machine.states[1].on_enter = [&]() { ++enterCount; };

    bool shouldRun = true;
    machine.add_transition("idle", "run", 0.1f, [&]() { return shouldRun; });

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.05f, skel, pose);
    expectTrue(exitCount == 1, "state machine calls on_exit when leaving");
    machine.evaluate(0.1f, skel, pose);
    expectTrue(enterCount == 1, "state machine calls on_enter when entered");
}

void testStateMachineCrossfadeClamp() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();

    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip run = makePositionClip(1, 4.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    machine.add_state("idle", std::move(idleNode));
    machine.add_state("run", std::move(runNode));

    bool shouldRun = true;
    machine.add_transition("idle", "run", 0.1f, [&]() { return shouldRun; });

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.05f, skel, pose);
    expectNear(machine.crossfade_alpha(), 0.5f, 1e-4f, "state machine crossfade alpha mid transition");

    machine.evaluate(1.f, skel, pose);
    expectNear(machine.crossfade_alpha(), 0.f, 1e-4f, "state machine crossfade alpha resets after complete");
    expectTrue(machine.active_state == 1u, "state machine completes transition after crossfade");
}

void testStateMachineTransition() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();

    fuse::animation::AnimationClip idle{};
    idle.duration = 1.f;
    fuse::animation::AnimationClip::BoneChannels idleChannel{};
    idleChannel.bone_index = 1;
    idleChannel.position.times = {0.f, 1.f};
    idleChannel.position.values_vec3 = {{0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}};
    idle.bone_channels.push_back(idleChannel);

    fuse::animation::AnimationClip run{};
    run.duration = 1.f;
    fuse::animation::AnimationClip::BoneChannels runChannel{};
    runChannel.bone_index = 1;
    runChannel.position.times = {0.f, 1.f};
    runChannel.position.values_vec3 = {{0.f, 4.f, 0.f, 0.f}, {0.f, 4.f, 0.f, 0.f}};
    run.bone_channels.push_back(runChannel);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    machine.add_state("idle", std::move(idleNode));
    machine.add_state("run", std::move(runNode));

    bool shouldRun = true;
    machine.add_transition("idle", "run", 0.2f, [&]() { return shouldRun; });

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.1f, skel, pose);
    machine.evaluate(0.2f, skel, pose);
    expectTrue(machine.active_state == 1u, "state machine transitions to run");
}

void testFabrikConverges() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);

    fuse::animation::FABRIKChain chain;
    chain.bone_indices = {0, 1};
    chain.target = {0.5f, 2.f, 0.f, 0.f};
    chain.max_iterations = 8;
    chain.solve(pose, skel);

    const fuse::animation::vec3 end = {
        pose.bone_world_transforms[1].data[12],
        pose.bone_world_transforms[1].data[13],
        pose.bone_world_transforms[1].data[14],
        0.f,
    };
    const fuse::f32 dx = end.x - chain.target.x;
    const fuse::f32 dy = end.y - chain.target.y;
    const fuse::f32 dz = end.z - chain.target.z;
    const fuse::f32 error = std::sqrt(dx * dx + dy * dy + dz * dz);
    expectTrue(error < 0.25f, "FABRIK moves end effector toward target");
}

void testSkinningCpuPath() {
#if defined(FUSE_HAS_CUDA)
    expectTrue(fuse::animation::skinning_backend() == fuse::animation::SkinningBackend::Cuda,
               "CUDA skinning backend when toolkit available");
#else
    expectTrue(fuse::animation::skinning_backend() == fuse::animation::SkinningBackend::CpuReference,
               "CPU reference skinning without CUDA toolkit");
#endif

    fuse::animation::SkinningInput input;
    input.rest_positions = {{0.f, 0.f, 0.f, 0.f}};
    input.rest_normals = {{0.f, 1.f, 0.f, 0.f}};
    input.weights.resize(1);
    input.weights[0].bone_indices[0] = 0;
    input.weights[0].bone_weights[0] = 1.f;

    fuse::animation::mat4 bone = fuse::animation::mat4::identity();
    bone.data[12] = 1.f;
    input.bone_transforms = {bone};

    fuse::animation::SkinningOutput output;
    expectTrue(fuse::animation::skin_vertices(input, output), "skin_vertices succeeds");
    expectNear(output.positions[0].x, 1.f, 1e-4f, "skinning applies bone transform");
}

void testPoseSoAHierarchy() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    expectTrue(pose.bone_count == 2u, "pose soa bone count");
    expectNear(pose.bone_world_transforms[1].data[13], 1.f, 1e-4f, "pose soa bind child offset");

    pose.local_positions[1] = {0.f, 2.f, 0.f, 0.f};
    pose.compute_world_transforms(skel);
    expectNear(pose.bone_world_transforms[1].data[13], 2.f, 1e-4f, "pose soa hierarchy propagation");
}

void testClipEvaluateLocalChannels() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip clip{};
    clip.duration = 1.f;
    clip.looping = false;

    fuse::animation::AnimationClip::BoneChannels channels{};
    channels.bone_index = 1;
    channels.position.times = {0.f, 1.f};
    channels.position.values_vec3 = {{0.f, 1.f, 0.f, 0.f}, {0.f, 2.f, 0.f, 0.f}};
    channels.rotation.times = {0.f, 1.f};
    channels.rotation.values_quat = {
        {0.f, 0.f, 0.f, 1.f},
        {0.f, 0.7071068f, 0.f, 0.7071068f},
    };
    clip.bone_channels.push_back(channels);

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    clip.evaluate(1.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 3.f, 1e-3f, "clip evaluate composes position onto bind local");
    expectNear(std::fabs(pose.bone_world_transforms[1].data[0]), std::fabs(pose.bone_world_transforms[1].data[10]),
               0.2f,
               "clip evaluate applies rotation channel");
}

void testBlendSpace1D() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();

    fuse::animation::AnimationClip walk{};
    walk.duration = 1.f;
    fuse::animation::AnimationClip::BoneChannels walkChannel{};
    walkChannel.bone_index = 1;
    walkChannel.position.times = {0.f, 1.f};
    walkChannel.position.values_vec3 = {{0.f, 1.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}};
    walk.bone_channels.push_back(walkChannel);

    fuse::animation::AnimationClip run{};
    run.duration = 1.f;
    fuse::animation::AnimationClip::BoneChannels runChannel{};
    runChannel.bone_index = 1;
    runChannel.position.times = {0.f, 1.f};
    runChannel.position.values_vec3 = {{0.f, 4.f, 0.f, 0.f}, {0.f, 4.f, 0.f, 0.f}};
    run.bone_channels.push_back(runChannel);

    fuse::animation::BlendSpace1D space;
    auto walkNode = std::make_unique<fuse::animation::ClipNode>();
    walkNode->clip = &walk;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    space.entries.push_back({0.f, std::move(walkNode)});
    space.entries.push_back({1.f, std::move(runNode)});

    fuse::f32 speed = 0.5f;
    space.param = &speed;

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    space.evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 3.5f, 0.05f, "blend space 1d interpolates composed clip poses");
}

void testLayeredBlendMask() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip baseClip = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip layerClip = makePositionClip(1, 5.f);

    auto baseNode = std::make_unique<fuse::animation::ClipNode>();
    baseNode->clip = &baseClip;
    auto layerNode = std::make_unique<fuse::animation::ClipNode>();
    layerNode->clip = &layerClip;

    fuse::animation::LayeredBlendNode layered;
    layered.base = std::move(baseNode);
    layered.layer = std::move(layerNode);
    layered.masked_bones = {1};
    layered.layer_weight = 0.5f;

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    layered.evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 4.f, 0.05f, "layered blend applies masked bone");
}

void testLayeredBlendWeights() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip baseClip = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip layerClip = makePositionClip(1, 5.f);

    auto makeLayered = [&](fuse::f32 weight) {
        fuse::animation::LayeredBlendNode layered;
        auto baseNode = std::make_unique<fuse::animation::ClipNode>();
        baseNode->clip = &baseClip;
        auto layerNode = std::make_unique<fuse::animation::ClipNode>();
        layerNode->clip = &layerClip;
        layered.base = std::move(baseNode);
        layered.layer = std::move(layerNode);
        layered.masked_bones = {1};
        layered.layer_weight = weight;
        return layered;
    };

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    makeLayered(0.f).evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 2.f, 0.05f, "layered blend weight zero keeps base");

    makeLayered(1.f).evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 6.f, 0.05f, "layered blend weight one takes layer");

    makeLayered(0.25f).evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 3.f, 0.05f, "layered blend weight quarter lerps local y");

    makeLayered(2.f).evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 6.f, 0.05f, "layered blend weight clamps above one");
}

void testLayeredBlendUnmaskedBone() {
    const fuse::animation::Skeleton skel = makeThreeBoneSkeleton();
    fuse::animation::AnimationClip baseClip = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip::BoneChannels baseGrandchild = {};
    baseGrandchild.bone_index = 2;
    baseGrandchild.position.values_vec3 = {{0.f, 1.f, 0.f, 0.f}};
    baseClip.bone_channels.push_back(baseGrandchild);

    fuse::animation::AnimationClip layerClip = makePositionClip(1, 8.f);
    fuse::animation::AnimationClip::BoneChannels layerGrandchild = {};
    layerGrandchild.bone_index = 2;
    layerGrandchild.position.values_vec3 = {{0.f, 8.f, 0.f, 0.f}};
    layerClip.bone_channels.push_back(layerGrandchild);

    auto makeLayeredNode = [&](fuse::f32 weight) {
        fuse::animation::LayeredBlendNode layered;
        auto baseNode = std::make_unique<fuse::animation::ClipNode>();
        baseNode->clip = &baseClip;
        auto layerNode = std::make_unique<fuse::animation::ClipNode>();
        layerNode->clip = &layerClip;
        layered.base = std::move(baseNode);
        layered.layer = std::move(layerNode);
        layered.masked_bones = {2};
        layered.layer_weight = weight;
        return layered;
    };

    fuse::animation::Pose baseOnly = fuse::animation::Pose::make_bind_pose(skel);
    makeLayeredNode(0.f).evaluate(0.f, skel, baseOnly);

    fuse::animation::Pose layerOnly = fuse::animation::Pose::make_bind_pose(skel);
    makeLayeredNode(1.f).evaluate(0.f, skel, layerOnly);

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    makeLayeredNode(1.f).evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], baseOnly.bone_world_transforms[1].data[13], 0.05f,
               "layered blend leaves unmasked parent at base");
    expectNear(pose.bone_world_transforms[2].data[13], layerOnly.bone_world_transforms[2].data[13], 0.05f,
               "layered blend applies masked grandchild");
}

void testPoseBufferClearReuse() {
    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::allocate(4);
    pose.resize(2);
    pose.local_positions[0] = {9.f, 8.f, 7.f, 0.f};
    pose.local_rotations[0] = {0.f, 1.f, 0.f, 0.f};
    pose.local_scales[0] = {2.f, 2.f, 2.f, 0.f};
    pose.bone_world_transforms[0] = fuse::animation::mat4::identity();
    pose.bone_world_transforms[0].data[12] = 42.f;

    pose.clear();
    expectTrue(pose.bone_count == 0u, "pose clear resets bone count");
    expectTrue(pose.local_positions.empty(), "pose clear empties local positions");
    expectTrue(pose.local_rotations.empty(), "pose clear empties local rotations");
    expectTrue(pose.local_scales.empty(), "pose clear empties local scales");
    expectTrue(pose.bone_world_transforms.empty(), "pose clear empties world transforms");

    pose.resize(2);
    expectTrue(pose.bone_count == 2u, "pose resize after clear restores bone count");
    expectTrue(pose.local_positions.size() == 2u, "pose resize after clear allocates columns");
    expectNear(pose.local_scales[1].y, 1.f, 1e-4f, "pose resize after clear resets default scale");

    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    pose.clear();
    pose.resize(static_cast<fuse::u32>(skel.bones.size()));
    pose.local_positions[1] = {0.f, 3.f, 0.f, 0.f};
    pose.compute_world_transforms(skel);
    expectNear(pose.bone_world_transforms[1].data[13], 3.f, 1e-4f, "pose buffer reuse after clear and resize");
}

void testPoseSoAToPoseRoundtrip() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA soa = fuse::animation::PoseSoA::from_bind_pose(skel);
    soa.local_positions[1] = {0.f, 4.f, 0.f, 0.f};
    soa.compute_world_transforms(skel);

    const fuse::animation::Pose pose = soa.to_pose();
    expectTrue(pose.bone_count == soa.bone_count, "pose soa to_pose preserves bone count");
    expectNear(pose.bone_world_transforms[1].data[13], 4.f, 1e-4f, "pose soa to_pose copies world transforms");
}

void testPoseSoAResizeDefaults() {
    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::allocate(4);
    pose.resize(3);
    expectTrue(pose.bone_count == 3u, "pose resize sets bone count");
    expectNear(pose.local_rotations[2].w, 1.f, 1e-4f, "pose resize seeds identity rotation");
    expectNear(pose.local_scales[2].x, 1.f, 1e-4f, "pose resize seeds unit scale");

    pose.resize(1);
    expectTrue(pose.bone_count == 1u, "pose shrink reduces bone count");
    expectTrue(pose.local_positions.size() == 1u, "pose shrink drops excess columns");
}

void testBlendPoseSoARotationScale() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA poseA = fuse::animation::PoseSoA::from_bind_pose(skel);
    fuse::animation::PoseSoA poseB = fuse::animation::PoseSoA::from_bind_pose(skel);
    poseA.local_scales[0] = {1.f, 1.f, 1.f, 0.f};
    poseB.local_scales[0] = {3.f, 3.f, 3.f, 0.f};
    poseB.local_rotations[0] = {0.f, 0.7071068f, 0.f, 0.7071068f};

    fuse::animation::PoseSoA out = fuse::animation::PoseSoA::allocate(2);
    fuse::animation::blend_pose_soa(poseA, poseB, 0.5f, out);
    expectNear(out.local_scales[0].x, 2.f, 1e-4f, "blend_pose_soa lerps local scale");
    expectNear(std::fabs(out.local_rotations[0].y), 0.3826834f, 0.05f, "blend_pose_soa lerps local rotation");
}

void testTwoBoneIKReachable() {
    const fuse::animation::Skeleton skel = makeLimbSkeleton();
    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);

    fuse::animation::TwoBoneIK ik;
    ik.root_bone = 0;
    ik.mid_bone = 1;
    ik.end_bone = 2;
    ik.target = {1.f, 1.f, 0.f, 0.f};
    ik.pole_vector = {0.f, 0.f, 1.f, 0.f};
    expectTrue(ik.solve(pose, skel), "two bone ik solves reachable target");

    const fuse::animation::vec3 end = {
        pose.bone_world_transforms[2].data[12],
        pose.bone_world_transforms[2].data[13],
        pose.bone_world_transforms[2].data[14],
        0.f,
    };
    const fuse::f32 error = std::sqrt((end.x - ik.target.x) * (end.x - ik.target.x) +
                                      (end.y - ik.target.y) * (end.y - ik.target.y) +
                                      (end.z - ik.target.z) * (end.z - ik.target.z));
    expectTrue(error < 0.05f, "two bone ik end effector reaches target");

    const fuse::f32 upperLen = 1.f;
    const fuse::f32 lowerLen = 1.f;
    const fuse::animation::vec3 mid = {
        pose.bone_world_transforms[1].data[12],
        pose.bone_world_transforms[1].data[13],
        pose.bone_world_transforms[1].data[14],
        0.f,
    };
    const fuse::animation::vec3 root = {0.f, 0.f, 0.f, 0.f};
    const fuse::f32 upperDist = std::sqrt((mid.x - root.x) * (mid.x - root.x) + (mid.y - root.y) * (mid.y - root.y) +
                                          (mid.z - root.z) * (mid.z - root.z));
    const fuse::f32 lowerDist = std::sqrt((end.x - mid.x) * (end.x - mid.x) + (end.y - mid.y) * (end.y - mid.y) +
                                          (end.z - mid.z) * (end.z - mid.z));
    expectNear(upperDist, upperLen, 0.05f, "two bone ik preserves upper length");
    expectNear(lowerDist, lowerLen, 0.05f, "two bone ik preserves lower length");
}

void testTwoBoneIKPoleBend() {
    const fuse::animation::Skeleton skel = makeLimbSkeleton();
    fuse::animation::Pose posePositive = fuse::animation::Pose::make_bind_pose(skel);
    fuse::animation::Pose poseNegative = fuse::animation::Pose::make_bind_pose(skel);

    fuse::animation::TwoBoneIK ikPositive;
    ikPositive.root_bone = 0;
    ikPositive.mid_bone = 1;
    ikPositive.end_bone = 2;
    ikPositive.target = {1.f, 1.f, 0.f, 0.f};
    ikPositive.pole_vector = {0.f, 0.f, 1.f, 0.f};

    fuse::animation::TwoBoneIK ikNegative = ikPositive;
    ikNegative.pole_vector = {0.f, 0.f, -1.f, 0.f};

    expectTrue(ikPositive.solve(posePositive, skel), "two bone ik pole +Z solves");
    expectTrue(ikNegative.solve(poseNegative, skel), "two bone ik pole -Z solves");

    const fuse::animation::vec3 midPositive = {
        posePositive.bone_world_transforms[1].data[12],
        posePositive.bone_world_transforms[1].data[13],
        posePositive.bone_world_transforms[1].data[14],
        0.f,
    };
    const fuse::animation::vec3 midNegative = {
        poseNegative.bone_world_transforms[1].data[12],
        poseNegative.bone_world_transforms[1].data[13],
        poseNegative.bone_world_transforms[1].data[14],
        0.f,
    };

    expectTrue(midPositive.z * midNegative.z < 0.f, "pole vector flips mid joint bend side");
    expectNear(midPositive.x, midNegative.x, 0.05f, "pole bend preserves mid joint forward offset");
}

void testTwoBoneIKUnreachableClamps() {
    const fuse::animation::Skeleton skel = makeLimbSkeleton();
    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);

    fuse::animation::TwoBoneIK ik;
    ik.root_bone = 0;
    ik.mid_bone = 1;
    ik.end_bone = 2;
    ik.target = {10.f, 0.f, 0.f, 0.f};
    ik.pole_vector = {0.f, 1.f, 0.f, 0.f};
    expectTrue(ik.solve(pose, skel), "two bone ik clamps unreachable target");

    const fuse::animation::vec3 end = {
        pose.bone_world_transforms[2].data[12],
        pose.bone_world_transforms[2].data[13],
        pose.bone_world_transforms[2].data[14],
        0.f,
    };
    const fuse::f32 reach = std::sqrt(end.x * end.x + end.y * end.y + end.z * end.z);
    expectNear(reach, 2.f, 0.05f, "two bone ik stretches to max reach when target is out of range");
}

void testTwoBoneIKSoA() {
    const fuse::animation::Skeleton skel = makeLimbSkeleton();
    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);

    fuse::animation::TwoBoneIK ik;
    ik.root_bone = 0;
    ik.mid_bone = 1;
    ik.end_bone = 2;
    ik.target = {0.5f, 1.5f, 0.f, 0.f};
    ik.pole_vector = {0.f, 0.f, 1.f, 0.f};
    expectTrue(ik.solve(pose, skel), "two bone ik soa path solves");

    pose.compute_world_transforms(skel);
    expectNear(pose.bone_world_transforms[2].data[12], ik.target.x, 0.05f, "two bone ik soa reaches target x");
    expectNear(pose.bone_world_transforms[2].data[13], ik.target.y, 0.05f, "two bone ik soa reaches target y");
}

void testTwoBoneIKEmptySkeleton() {
    const fuse::animation::Skeleton empty{};
    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(empty);
    fuse::animation::PoseSoA poseSoa = fuse::animation::PoseSoA::from_bind_pose(empty);

    fuse::animation::TwoBoneIK ik;
    ik.root_bone = 0;
    ik.mid_bone = 1;
    ik.end_bone = 2;
    ik.target = {1.f, 1.f, 0.f, 0.f};
    expectTrue(!ik.has_valid_chain(empty), "two bone ik rejects empty skeleton");
    expectTrue(!ik.solve(pose, empty), "two bone ik aos rejects empty skeleton");
    expectTrue(!ik.solve(poseSoa, empty), "two bone ik soa rejects empty skeleton");

    fuse::animation::FABRIKChain chain;
    chain.bone_indices = {0, 1};
    chain.target = {0.f, 1.f, 0.f, 0.f};
    chain.solve(pose, empty);
    expectTrue(pose.bone_count == 0u, "fabrik on empty skeleton leaves pose empty");
}

void testTwoBoneIKInPlace() {
    const fuse::animation::Skeleton skel = makeLimbSkeleton();
    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    pose.bone_world_transforms[0].data[12] = 3.f;

    fuse::animation::TwoBoneIK ik;
    ik.root_bone = 0;
    ik.mid_bone = 1;
    ik.end_bone = 2;
    ik.target = {4.f, 1.f, 0.f, 0.f};
    ik.pole_vector = {0.f, 0.f, 1.f, 0.f};
    expectTrue(ik.solve(pose, skel), "two bone ik solves in place on existing pose");

    expectNear(pose.bone_world_transforms[0].data[12], 3.f, 1e-4f, "two bone ik preserves root translation");
    const fuse::animation::vec3 end = {
        pose.bone_world_transforms[2].data[12],
        pose.bone_world_transforms[2].data[13],
        pose.bone_world_transforms[2].data[14],
        0.f,
    };
    const fuse::f32 error = std::sqrt((end.x - ik.target.x) * (end.x - ik.target.x) +
                                      (end.y - ik.target.y) * (end.y - ik.target.y) +
                                      (end.z - ik.target.z) * (end.z - ik.target.z));
    expectTrue(error < 0.05f, "two bone ik in place reaches target from offset root");
}

void testTwoBoneIKChainOrder() {
    const fuse::animation::Skeleton skel = makeLimbSkeleton();

    fuse::animation::TwoBoneIK duplicateMid;
    duplicateMid.root_bone = 0;
    duplicateMid.mid_bone = 0;
    duplicateMid.end_bone = 1;
    expectTrue(!duplicateMid.has_valid_chain(skel), "two bone ik rejects duplicate bone indices");

    fuse::animation::TwoBoneIK wrongOrder;
    wrongOrder.root_bone = 0;
    wrongOrder.mid_bone = 2;
    wrongOrder.end_bone = 1;
    expectTrue(!wrongOrder.has_valid_chain(skel), "two bone ik rejects non-chain bone order");

    fuse::animation::TwoBoneIK outOfRange;
    outOfRange.root_bone = 0;
    outOfRange.mid_bone = 1;
    outOfRange.end_bone = 99;
    expectTrue(!outOfRange.has_valid_chain(skel), "two bone ik rejects out of range end bone");
}

void testTwoBoneIKMaxReach() {
    const fuse::animation::Skeleton skel = makeLimbSkeleton();
    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);

    fuse::animation::TwoBoneIK ik;
    ik.root_bone = 0;
    ik.mid_bone = 1;
    ik.end_bone = 2;
    ik.reach_epsilon = 0.001f;
    expectNear(ik.max_reach(pose), 1.999f, 0.01f, "two bone ik max reach from bind pose segment lengths");

    ik.target = {10.f, 0.f, 0.f, 0.f};
    ik.pole_vector = {0.f, 1.f, 0.f, 0.f};
    expectTrue(ik.solve(pose, skel), "two bone ik clamps unreachable target using max reach");

    const fuse::animation::vec3 end = {
        pose.bone_world_transforms[2].data[12],
        pose.bone_world_transforms[2].data[13],
        pose.bone_world_transforms[2].data[14],
        0.f,
    };
    const fuse::f32 reach = std::sqrt(end.x * end.x + end.y * end.y + end.z * end.z);
    expectNear(reach, ik.max_reach(pose), 0.05f, "two bone ik clamped end matches max reach");
}

void testRetargetMapBuildByName() {
    const fuse::animation::Skeleton source = makeTwoBoneSkeleton();
    const fuse::animation::Skeleton target = makeRetargetTargetSkeleton();

    const fuse::animation::RetargetMap map = fuse::animation::RetargetMap::build_by_name(source, target);
    expectTrue(map.is_valid(), "retarget map is valid after name pairing");
    expectTrue(map.bone_map.size() == 2u, "retarget map pairs shared bone names only");
    expectTrue(map.source_bone_count == 2u, "retarget map records source bone count");
    expectTrue(map.target_bone_count == 3u, "retarget map records target bone count");
}

void testRetargetApplyPoseSoA() {
    const fuse::animation::Skeleton sourceSkel = makeTwoBoneSkeleton();
    const fuse::animation::Skeleton targetSkel = makeRetargetTargetSkeleton();
    const fuse::animation::RetargetMap map = fuse::animation::RetargetMap::build_by_name(sourceSkel, targetSkel);

    fuse::animation::PoseSoA sourcePose = fuse::animation::PoseSoA::from_bind_pose(sourceSkel);
    sourcePose.local_positions[1] = {0.f, 6.f, 0.f, 0.f};
    sourcePose.compute_world_transforms(sourceSkel);

    fuse::animation::PoseSoA targetPose = fuse::animation::PoseSoA::from_bind_pose(targetSkel);
    map.apply_pose_soa(sourcePose, targetSkel, targetPose);
    expectNear(targetPose.local_positions[1].y, 6.f, 1e-4f, "retarget copies mapped local translation");
    expectNear(targetPose.bone_world_transforms[2].data[12], 2.f, 1e-4f, "retarget leaves unmapped prop bone at bind");
}

void testRetargetApplyPose() {
    const fuse::animation::Skeleton sourceSkel = makeTwoBoneSkeleton();
    const fuse::animation::Skeleton targetSkel = makeRetargetTargetSkeleton();
    const fuse::animation::RetargetMap map = fuse::animation::RetargetMap::build_by_name(sourceSkel, targetSkel);

    fuse::animation::Pose sourcePose = fuse::animation::Pose::make_bind_pose(sourceSkel);
    sourcePose.bone_world_transforms[1].data[13] = 6.f;

    fuse::animation::Pose targetPose = fuse::animation::Pose::make_bind_pose(targetSkel);
    map.apply_pose(sourcePose, targetSkel, targetPose);
    expectNear(targetPose.bone_world_transforms[1].data[13], 6.f, 1e-4f, "retarget apply_pose copies mapped world transform");
    expectNear(targetPose.bone_world_transforms[2].data[12], 2.f, 1e-4f, "retarget apply_pose leaves unmapped bones at bind");
}

void testRetargetEmptySkeleton() {
    const fuse::animation::Skeleton empty{};
    const fuse::animation::RetargetMap byName = fuse::animation::RetargetMap::build_by_name(empty, empty);
    const fuse::animation::RetargetMap identity = fuse::animation::RetargetMap::build_identity(empty);
    expectTrue(!byName.is_valid(), "retarget build_by_name on empty skeleton is invalid");
    expectTrue(!identity.is_valid(), "retarget build_identity on empty skeleton is invalid");
    expectTrue(identity.mapped_bone_count() == 0u, "retarget identity on empty skeleton maps zero bones");

    fuse::animation::PoseSoA poseSoa = fuse::animation::PoseSoA::allocate(2);
    byName.apply_pose_soa(poseSoa, empty, poseSoa);
    expectTrue(poseSoa.bone_count == 0u, "retarget apply_pose_soa on empty skeleton clears output");

    fuse::animation::Pose pose{};
    byName.apply_pose(pose, empty, pose);
    expectTrue(pose.bone_count == 0u, "retarget apply_pose on empty skeleton clears output");
}

void testRetargetIdentity() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    const fuse::animation::RetargetMap map = fuse::animation::RetargetMap::build_identity(skel);
    expectTrue(map.is_valid(), "retarget identity map is valid");
    expectTrue(map.mapped_bone_count() == skel.bone_count, "retarget identity maps every bone");
    expectTrue(map.find_source_bone(1) == 1, "retarget find_source_bone returns mapped index");
    expectTrue(map.find_source_bone(99) == -1, "retarget find_source_bone returns -1 when unmapped");

    fuse::animation::PoseSoA sourcePose = fuse::animation::PoseSoA::from_bind_pose(skel);
    sourcePose.local_positions[1] = {0.f, 6.f, 0.f, 0.f};
    sourcePose.local_scales[0] = {2.f, 2.f, 2.f, 0.f};
    sourcePose.compute_world_transforms(skel);

    fuse::animation::PoseSoA targetPose = fuse::animation::PoseSoA::from_bind_pose(skel);
    map.apply_pose_soa(sourcePose, skel, targetPose);
    expectNear(targetPose.local_positions[1].y, 6.f, 1e-4f, "retarget identity preserves local translation");
    expectNear(targetPose.local_scales[0].x, 2.f, 1e-4f, "retarget identity preserves local scale");
    expectNear(targetPose.bone_world_transforms[1].data[13], sourcePose.bone_world_transforms[1].data[13], 1e-4f,
               "retarget identity preserves world transforms");

    fuse::animation::Pose sourceAoS = sourcePose.to_pose();
    fuse::animation::Pose targetAoS = fuse::animation::Pose::make_bind_pose(skel);
    map.apply_pose(sourceAoS, skel, targetAoS);
    expectNear(targetPose.bone_world_transforms[1].data[13], targetAoS.bone_world_transforms[1].data[13], 1e-4f,
               "retarget identity apply_pose matches soa world transform");
}

void testRetargetFindTargetBone() {
    const fuse::animation::Skeleton source = makeTwoBoneSkeleton();
    const fuse::animation::Skeleton target = makeRetargetTargetSkeleton();
    const fuse::animation::RetargetMap map = fuse::animation::RetargetMap::build_by_name(source, target);
    expectTrue(map.find_target_bone(0) == 0, "retarget find_target_bone maps root");
    expectTrue(map.find_target_bone(1) == 1, "retarget find_target_bone maps child");
    expectTrue(map.find_target_bone(99) == -1, "retarget find_target_bone returns -1 when unmapped");
}

void testRetargetTranslationScale() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::RetargetMap map = fuse::animation::RetargetMap::build_identity(skel);
    map.bone_map[1].translation_scale = 0.5f;

    fuse::animation::PoseSoA sourcePose = fuse::animation::PoseSoA::from_bind_pose(skel);
    sourcePose.local_positions[1] = {0.f, 8.f, 0.f, 0.f};
    sourcePose.compute_world_transforms(skel);

    fuse::animation::PoseSoA targetPose = fuse::animation::PoseSoA::from_bind_pose(skel);
    map.apply_pose_soa(sourcePose, skel, targetPose);
    expectNear(targetPose.local_positions[1].y, 4.f, 1e-4f, "retarget translation scale halves mapped translation");
}

void testBlendPoseSoAReuse() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA poseA = fuse::animation::PoseSoA::from_bind_pose(skel);
    fuse::animation::PoseSoA poseB = fuse::animation::PoseSoA::from_bind_pose(skel);
    poseA.local_positions[1] = {0.f, 1.f, 0.f, 0.f};
    poseB.local_positions[1] = {0.f, 5.f, 0.f, 0.f};

    fuse::animation::PoseSoA out = fuse::animation::PoseSoA::allocate(2);
    fuse::animation::blend_pose_soa(poseA, poseB, 0.5f, out);
    expectNear(out.local_positions[1].y, 3.f, 1e-4f, "blend_pose_soa lerps local position");

    fuse::animation::blend_pose_soa(poseA, poseB, 0.f, out);
    expectNear(out.local_positions[1].y, 1.f, 1e-4f, "blend_pose_soa reuses output buffer at weight zero");
    expectTrue(out.bone_count == 2u, "blend_pose_soa reuse preserves bone count");

    fuse::animation::blend_pose_soa(poseA, poseB, 1.f, out);
    expectNear(out.local_positions[1].y, 5.f, 1e-4f, "blend_pose_soa reuses output buffer at weight one");
}

void testEmptyBlendSpace1D() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::BlendSpace1D space;

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    space.evaluate(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 1.f, 1e-4f, "empty blend space 1d returns bind pose");

    const fuse::animation::BlendSpace1DSample sample = fuse::animation::sample_blend_space_1d(space, 0.5f);
    expectTrue(sample.lower_index == 0u, "empty blend space 1d sample zeroes bracket");
    expectNear(sample.alpha, 0.f, 1e-4f, "empty blend space 1d sample alpha is zero");
}

void testEmptyBlendSpace2D() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::BlendSpace2D space;

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    space.evaluate_soa(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 1.f, 1e-4f, "empty blend space 2d returns bind pose");

    const fuse::animation::BlendSpace2DSample sample =
        fuse::animation::sample_blend_space_2d(space, {0.5f, 0.5f});
    expectTrue(sample.weights.empty(), "empty blend space 2d sample has no weights");
}

void testEmptyStateMachine() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimStateMachine machine;

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.1f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 1.f, 1e-4f, "empty state machine returns bind pose");
    expectNear(machine.crossfade_alpha(), 0.f, 1e-4f, "empty state machine crossfade alpha is zero");
}

void testBlendNode2WeightClamp() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip clipA = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip clipB = makePositionClip(1, 5.f);

    fuse::f32 negativeBlend = -0.5f;
    fuse::animation::BlendNode2 negativeNode;
    {
        auto nodeA = std::make_unique<fuse::animation::ClipNode>();
        nodeA->clip = &clipA;
        auto nodeB = std::make_unique<fuse::animation::ClipNode>();
        nodeB->clip = &clipB;
        negativeNode.a = std::move(nodeA);
        negativeNode.b = std::move(nodeB);
    }
    negativeNode.blend_param = &negativeBlend;

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    negativeNode.evaluate_soa(0.f, skel, pose);
    expectNear(pose.local_positions[1].y, 2.f, 1e-4f, "blend node clamps negative weight to zero");

    fuse::f32 overBlend = 2.f;
    fuse::animation::BlendNode2 overNode;
    {
        auto nodeA = std::make_unique<fuse::animation::ClipNode>();
        nodeA->clip = &clipA;
        auto nodeB = std::make_unique<fuse::animation::ClipNode>();
        nodeB->clip = &clipB;
        overNode.a = std::move(nodeA);
        overNode.b = std::move(nodeB);
    }
    overNode.blend_param = &overBlend;
    overNode.evaluate_soa(0.f, skel, pose);
    expectNear(pose.local_positions[1].y, 6.f, 1e-4f, "blend node clamps weight above one");
}

void testBlendNode2NullChildren() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::BlendNode2 blendNode;

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    blendNode.evaluate_soa(0.f, skel, pose);
    expectNear(pose.local_positions[1].y, 1.f, 1e-4f, "blend node with null children returns bind pose");
}

void testStateMachineInitialOnEnter() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    machine.add_state("idle", std::move(idleNode));

    int enterCount = 0;
    machine.states[0].on_enter = [&]() { ++enterCount; };

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.f, skel, pose);
    expectTrue(enterCount == 1, "state machine calls on_enter for initial state on first evaluate");
}

void testStateMachineZeroBlendDuration() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip run = makePositionClip(1, 6.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    machine.add_state("idle", std::move(idleNode));
    machine.add_state("run", std::move(runNode));

    bool shouldRun = true;
    machine.add_transition("idle", "run", 0.f, [&]() { return shouldRun; });

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.f, skel, pose);
    expectTrue(machine.active_state == 1u, "zero blend duration transitions instantly");
    expectNear(machine.crossfade_alpha(), 0.f, 1e-4f, "zero blend duration resets crossfade alpha");
    expectNear(pose.bone_world_transforms[1].data[13], 7.f, 0.05f, "zero blend duration snaps to target pose");
}

void testStateMachineIgnoresSelfTransition() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    machine.add_state("idle", std::move(idleNode));

    machine.add_transition("idle", "idle", 0.1f, []() { return true; });
    expectTrue(machine.transitions.empty(), "self transition is not registered");

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.1f, skel, pose);
    expectTrue(machine.active_state == 0u, "state machine stays in place without self transition");
    expectTrue(!machine.is_transitioning, "state machine does not enter crossfade for self transition");
}

void testStateMachineInvalidTransitionIgnored() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    machine.add_state("idle", std::move(idleNode));

    machine.add_transition("missing", "idle", 0.1f, []() { return true; });
    machine.add_transition("idle", "missing", 0.1f, []() { return true; });
    expectTrue(machine.transitions.empty(), "invalid transition names are ignored");
}

void testStateMachineNoInterruptDuringCrossfade() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip run = makePositionClip(1, 4.f);
    fuse::animation::AnimationClip jump = makePositionClip(1, 8.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    auto jumpNode = std::make_unique<fuse::animation::ClipNode>();
    jumpNode->clip = &jump;
    machine.add_state("idle", std::move(idleNode));
    machine.add_state("run", std::move(runNode));
    machine.add_state("jump", std::move(jumpNode));

    bool shouldRun = true;
    bool shouldJump = true;
    machine.add_transition("idle", "run", 0.2f, [&]() { return shouldRun; });
    machine.add_transition("run", "jump", 0.2f, [&]() { return shouldJump; });

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.05f, skel, pose);
    expectTrue(machine.is_transitioning, "state machine begins crossfade to run");
    expectTrue(machine.pending_state == 1u, "state machine targets run during crossfade");

    machine.evaluate(0.05f, skel, pose);
    expectTrue(machine.pending_state == 1u, "state machine does not retarget mid crossfade");
    expectTrue(machine.is_transitioning, "state machine remains in crossfade");
}

void testAccumulateWeightedPoseSoA() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA poseA = fuse::animation::PoseSoA::from_bind_pose(skel);
    fuse::animation::PoseSoA poseB = fuse::animation::PoseSoA::from_bind_pose(skel);
    poseA.local_positions[1] = {0.f, 2.f, 0.f, 0.f};
    poseB.local_positions[1] = {0.f, 6.f, 0.f, 0.f};

    fuse::animation::PoseSoA result = fuse::animation::PoseSoA::from_bind_pose(skel);
    fuse::f32 totalWeight = 0.f;
    fuse::animation::accumulate_weighted_pose_soa(result, totalWeight, poseA, 1.f);
    fuse::animation::accumulate_weighted_pose_soa(result, totalWeight, poseB, 1.f);
    expectNear(result.local_positions[1].y, 4.f, 1e-4f, "accumulate weighted pose soa equal weights average");
    expectNear(totalWeight, 2.f, 1e-4f, "accumulate weighted pose soa tracks total weight");
}

void testCopyPoseSoALocal() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA src = fuse::animation::PoseSoA::from_bind_pose(skel);
    src.local_positions[1] = {0.f, 7.f, 0.f, 0.f};
    src.local_scales[0] = {2.f, 2.f, 2.f, 0.f};

    fuse::animation::PoseSoA dst = fuse::animation::PoseSoA::allocate(2);
    fuse::animation::copy_pose_soa_local(src, dst);
    expectNear(dst.local_positions[1].y, 7.f, 1e-4f, "copy pose soa local copies positions");
    expectNear(dst.local_scales[0].x, 2.f, 1e-4f, "copy pose soa local copies scales");
    expectTrue(dst.bone_count == src.bone_count, "copy pose soa local matches bone count");
}

void testFinalizeWeightedPoseSoAEmpty() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA result = fuse::animation::PoseSoA::from_bind_pose(skel);
    result.local_positions[1] = {0.f, 9.f, 0.f, 0.f};

    fuse::animation::PoseSoA out = fuse::animation::PoseSoA::allocate(2);
    fuse::animation::finalize_weighted_pose_soa(result, 0.f, skel, out);
    expectNear(out.bone_world_transforms[1].data[13], 1.f, 1e-4f,
               "finalize weighted pose soa with zero weight returns bind pose");
}

void testFinalizeWeightedPoseSoARecompute() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA result = fuse::animation::PoseSoA::from_bind_pose(skel);
    result.local_positions[1] = {0.f, 5.f, 0.f, 0.f};

    fuse::animation::PoseSoA out = fuse::animation::PoseSoA::allocate(2);
    fuse::animation::finalize_weighted_pose_soa(result, 1.f, skel, out);
    expectNear(out.bone_world_transforms[1].data[13], 5.f, 1e-4f,
               "finalize weighted pose soa recomputes world transforms");
}

void testAccumulateWeightedPoseSoASkipsZeroWeight() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::PoseSoA poseA = fuse::animation::PoseSoA::from_bind_pose(skel);
    poseA.local_positions[1] = {0.f, 2.f, 0.f, 0.f};

    fuse::animation::PoseSoA result = fuse::animation::PoseSoA::from_bind_pose(skel);
    fuse::f32 totalWeight = 0.f;
    fuse::animation::accumulate_weighted_pose_soa(result, totalWeight, poseA, 0.f);
    expectNear(totalWeight, 0.f, 1e-4f, "accumulate weighted pose soa skips zero weight");
    expectNear(result.local_positions[1].y, 1.f, 1e-4f,
               "accumulate weighted pose soa leaves bind pose when weight is zero");
}

void testEmptyClipNode() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::ClipNode node;

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    node.evaluate_soa(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 1.f, 1e-4f, "null clip node returns bind pose");
}

void testEmptyBlendSpace1DSoA() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::BlendSpace1D space;

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    space.evaluate_soa(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 1.f, 1e-4f, "empty blend space 1d soa returns bind pose");
}

void testAdditiveBlendWeightClamp() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip baseClip = makePositionClip(1, 2.f);
    fuse::animation::AnimationClip layerClip = makePositionClip(1, 8.f);

    auto makeAdditive = [&](fuse::f32 weight) {
        fuse::animation::AdditiveBlendNode additive;
        auto baseNode = std::make_unique<fuse::animation::ClipNode>();
        baseNode->clip = &baseClip;
        auto layerNode = std::make_unique<fuse::animation::ClipNode>();
        layerNode->clip = &layerClip;
        additive.base = std::move(baseNode);
        additive.layer = std::move(layerNode);
        additive.masked_bones = {1};
        additive.layer_weight = weight;
        return additive;
    };

    fuse::animation::PoseSoA pose = fuse::animation::PoseSoA::from_bind_pose(skel);
    makeAdditive(0.f).evaluate_soa(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 3.f, 0.05f, "additive blend weight zero keeps base");

    makeAdditive(1.f).evaluate_soa(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 11.f, 0.05f, "additive blend weight one applies full delta");

    makeAdditive(2.f).evaluate_soa(0.f, skel, pose);
    expectNear(pose.bone_world_transforms[1].data[13], 11.f, 0.05f, "additive blend weight clamps above one");
}

void testStateMachineFindStateAndEdges() {
    fuse::animation::AnimStateMachine machine;
    machine.add_state("idle", nullptr);
    machine.add_state("run", nullptr);
    machine.add_state("jump", nullptr);
    machine.add_transition("idle", "run", 0.2f, []() { return false; });
    machine.add_transition("idle", "jump", 0.2f, []() { return false; });
    machine.add_transition("run", "idle", 0.2f, []() { return false; });

    expectTrue(machine.find_state_index("idle") == 0, "find state index locates idle");
    expectTrue(machine.find_state_index("missing") < 0, "find state index rejects unknown name");
    expectTrue(machine.outgoing_transition_count(0u) == 2u, "outgoing transition count from idle");
    expectTrue(machine.has_transition(0u, 1u), "has transition detects idle to run edge");
    expectTrue(!machine.has_transition(1u, 2u), "has transition rejects missing edge");
}

void testStateMachineConditionFalse() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip run = makePositionClip(1, 6.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    machine.add_state("idle", std::move(idleNode));
    machine.add_state("run", std::move(runNode));
    machine.add_transition("idle", "run", 0.2f, []() { return false; });

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.1f, skel, pose);
    expectTrue(machine.active_state == 0u, "state machine stays idle when condition is false");
    expectTrue(!machine.is_transitioning, "state machine does not crossfade on false condition");
}

void testStateMachineFirstTransitionWins() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip run = makePositionClip(1, 4.f);
    fuse::animation::AnimationClip jump = makePositionClip(1, 8.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    auto jumpNode = std::make_unique<fuse::animation::ClipNode>();
    jumpNode->clip = &jump;
    machine.add_state("idle", std::move(idleNode));
    machine.add_state("run", std::move(runNode));
    machine.add_state("jump", std::move(jumpNode));

    machine.add_transition("idle", "run", 0.f, []() { return true; });
    machine.add_transition("idle", "jump", 0.f, []() { return true; });

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.f, skel, pose);
    expectTrue(machine.active_state == 1u, "state machine takes first matching transition");
    expectTrue(machine.has_transition(0u, 1u), "first transition edge remains registered");
}

void testStateMachineReset() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::AnimationClip idle = makePositionClip(1, 1.f);
    fuse::animation::AnimationClip run = makePositionClip(1, 6.f);

    fuse::animation::AnimStateMachine machine;
    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    idleNode->clip = &idle;
    auto runNode = std::make_unique<fuse::animation::ClipNode>();
    runNode->clip = &run;
    machine.add_state("idle", std::move(idleNode));
    machine.add_state("run", std::move(runNode));

    int enterCount = 0;
    int exitCount = 0;
    machine.states[0].on_enter = [&]() { ++enterCount; };
    machine.states[0].on_exit = [&]() { ++exitCount; };
    machine.states[1].on_enter = [&]() { ++enterCount; };

    bool shouldRun = true;
    machine.add_transition("idle", "run", 0.2f, [&]() { return shouldRun; });

    fuse::animation::Pose pose = fuse::animation::Pose::make_bind_pose(skel);
    machine.evaluate(0.05f, skel, pose);
    expectTrue(machine.is_transitioning, "state machine is mid crossfade before reset");

    machine.reset();
    expectTrue(machine.active_state == 0u, "reset returns to first state");
    expectTrue(!machine.is_transitioning, "reset clears crossfade flag");
    expectNear(machine.crossfade_alpha(), 0.f, 1e-4f, "reset clears crossfade alpha");
    expectTrue(exitCount == 1, "reset does not invoke additional on_exit");

    machine.evaluate(0.f, skel, pose);
    expectTrue(enterCount == 2, "reset allows initial on_enter to fire again");
}

void testAnimatorTick() {
    const fuse::animation::Skeleton skel = makeTwoBoneSkeleton();
    fuse::animation::Animator animator;
    animator.playback_rate = 1.f;

    auto idleNode = std::make_unique<fuse::animation::ClipNode>();
    fuse::animation::AnimationClip idle{};
    idle.duration = 1.f;
    idleNode->clip = &idle;
    animator.state_machine = std::make_unique<fuse::animation::AnimStateMachine>();
    animator.state_machine->add_state("idle", std::move(idleNode));

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    animator.tick(skel, ctx);
    animator.tick(skel, ctx);
    expectTrue(animator.tick_count == 2u, "animator tick count advances");
    expectTrue(animator.current_pose.bone_count == 2u, "animator stores pose");
}

} // namespace

int main() {
    fuse::core::initialize();
    testSkeletonHierarchy();
    testPoseSoAHierarchy();
    testClipSampling();
    testClipEvaluateLocalChannels();
    testBlendNodeInterpolation();
    testBlendSpace1D();
    testBlendSpace1DParameterSample();
    testBlendSpace2D();
    testBlendSpace2DParameterSample();
    testAdditiveBlendLayer();
    testBlendTreeEvaluateSoA();
    testLayeredBlendMask();
    testLayeredBlendWeights();
    testLayeredBlendUnmaskedBone();
    testPoseBufferClearReuse();
    testPoseSoAToPoseRoundtrip();
    testPoseSoAResizeDefaults();
    testBlendPoseSoARotationScale();
    testBlendPoseSoAReuse();
    testTwoBoneIKReachable();
    testTwoBoneIKPoleBend();
    testTwoBoneIKUnreachableClamps();
    testTwoBoneIKSoA();
    testTwoBoneIKEmptySkeleton();
    testTwoBoneIKInPlace();
    testTwoBoneIKChainOrder();
    testTwoBoneIKMaxReach();
    testRetargetMapBuildByName();
    testRetargetApplyPoseSoA();
    testRetargetApplyPose();
    testRetargetEmptySkeleton();
    testRetargetIdentity();
    testRetargetFindTargetBone();
    testRetargetTranslationScale();
    testEmptyBlendSpace1D();
    testEmptyBlendSpace2D();
    testEmptyStateMachine();
    testBlendNode2WeightClamp();
    testBlendNode2NullChildren();
    testAccumulateWeightedPoseSoA();
    testFinalizeWeightedPoseSoAEmpty();
    testFinalizeWeightedPoseSoARecompute();
    testAccumulateWeightedPoseSoASkipsZeroWeight();
    testCopyPoseSoALocal();
    testEmptyClipNode();
    testEmptyBlendSpace1DSoA();
    testAdditiveBlendWeightClamp();
    testStateMachineFindStateAndEdges();
    testStateMachineConditionFalse();
    testStateMachineFirstTransitionWins();
    testStateMachineReset();
    testStateMachineEnterExit();
    testStateMachineCrossfadeClamp();
    testStateMachineTransition();
    testStateMachineInitialOnEnter();
    testStateMachineZeroBlendDuration();
    testStateMachineIgnoresSelfTransition();
    testStateMachineInvalidTransitionIgnored();
    testStateMachineNoInterruptDuringCrossfade();
    testFabrikConverges();
    testSkinningCpuPath();
    testAnimatorTick();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_animation_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_animation_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
