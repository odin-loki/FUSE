#include <fuse/animation/animator.hpp>
#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/clip.hpp>
#include <fuse/animation/ik_solver.hpp>
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
    testClipSampling();
    testBlendNodeInterpolation();
    testStateMachineTransition();
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
