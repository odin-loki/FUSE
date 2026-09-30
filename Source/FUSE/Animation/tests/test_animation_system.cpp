// MP-B7.1-PARALLEL-EVAL gate: AnimationSystem::update evaluates 1000 Animators (state machine with a
// BlendNode2 state and a clip state, crossfade transitions, two-bone IK on a third of them, FK and the
// skinning palette) in parallel on JobScheduler workers. Checks:
//   * parallel == serial, bit-identical, every frame (two identical registries, one updated with
//     update(), one with update_serial()),
//   * the result does not depend on the worker count (1 vs 3 workers),
//   * zero heap allocations per steady-state parallel update (global operator new, every thread).

#include <fuse/animation/animation_system.hpp>
#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/clip.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace {
std::atomic<unsigned long long> g_allocations{0};
std::atomic<bool> g_counting{false};

void* countedAlloc(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

} // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using fuse::f32;
using fuse::u32;
using fuse::usize;
namespace anim = fuse::animation;
namespace ecs = fuse::ecs;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kAnimators = 1000;
constexpr u32 kBones = 10;
constexpr f32 kDt = 1.f / 60.f;

struct SharedAssets {
    anim::Skeleton skeleton;
    anim::AnimationClip walk;
    anim::AnimationClip wave;
};

anim::quat axisQuat(f32 x, f32 y, f32 z, f32 radians) {
    const f32 s = std::sin(radians * 0.5f);
    return {x * s, y * s, z * s, std::cos(radians * 0.5f)};
}

void buildAssets(SharedAssets& a) {
    a.skeleton.bones.resize(kBones);
    a.skeleton.bone_count = kBones;
    for (u32 i = 0; i < kBones; ++i) {
        anim::Bone& bone = a.skeleton.bones[i];
        std::snprintf(bone.name, sizeof(bone.name), "bone%u", i);
        bone.parent_index = static_cast<fuse::s32>(i) - 1;
        bone.local_transform = anim::mat4_from_trs({0.f, i == 0 ? 0.f : 0.4f, 0.f, 0.f}, {}, {1.f, 1.f, 1.f, 0.f});
    }
    const anim::Pose bind = anim::Pose::make_bind_pose(a.skeleton);
    for (u32 i = 0; i < kBones; ++i) {
        a.skeleton.bones[i].inverse_bind = anim::mat4_inverse_affine(bind.bone_world_transforms[i]);
    }
    auto fill = [](anim::AnimationClip& clip, f32 duration, f32 amplitude, f32 ax, f32 az) {
        clip.duration = duration;
        clip.looping = true;
        for (u32 i = 0; i < kBones; ++i) {
            anim::AnimationClip::BoneChannels ch{};
            ch.bone_index = i;
            ch.rotation.times = {0.f, duration * 0.5f, duration};
            const f32 angle = amplitude * (1.f + 0.1f * static_cast<f32>(i));
            ch.rotation.values_quat = {axisQuat(ax, 0.f, az, -angle), axisQuat(ax, 0.f, az, angle),
                                       axisQuat(ax, 0.f, az, -angle)};
            ch.position.times = {0.f, duration};
            ch.position.values_vec3 = {{0.f, i == 0 ? 0.f : 0.4f, 0.f, 0.f}, {0.f, i == 0 ? 0.f : 0.45f, 0.f, 0.f}};
            clip.bone_channels.push_back(ch);
        }
    };
    fill(a.walk, 1.2f, 0.3f, 0.f, 1.f);
    fill(a.wave, 0.8f, 0.5f, 1.f, 0.f);
}

/// Per-animator parameters with stable addresses (blend weights and transition flags).
struct AnimatorParams {
    std::vector<f32> blend;
    std::vector<int> goWave;
};

anim::Animator makeAnimator(const SharedAssets& a, AnimatorParams& params, u32 i) {
    anim::Animator animator;
    animator.playback_rate = 0.75f + 0.5f * static_cast<f32>(i % 7u) / 7.f;
    animator.state_machine = std::make_unique<anim::AnimStateMachine>();
    auto blend = std::make_unique<anim::BlendNode2>();
    auto walkNode = std::make_unique<anim::ClipNode>();
    walkNode->clip = &a.walk;
    walkNode->time = 0.013f * static_cast<f32>(i);
    auto waveNode = std::make_unique<anim::ClipNode>();
    waveNode->clip = &a.wave;
    blend->a = std::move(walkNode);
    blend->b = std::move(waveNode);
    blend->blend_param = &params.blend[i];
    animator.state_machine->add_state("locomotion", std::move(blend));
    auto waveOnly = std::make_unique<anim::ClipNode>();
    waveOnly->clip = &a.wave;
    animator.state_machine->add_state("wave", std::move(waveOnly));
    int* flag = &params.goWave[i];
    animator.state_machine->add_transition("locomotion", "wave", 0.25f, [flag]() { return *flag != 0; });
    animator.state_machine->add_transition("wave", "locomotion", 0.2f, [flag]() { return *flag == 0; });
    return animator;
}

struct World {
    ecs::Registry registry;
    anim::AnimationSystem system;
    AnimatorParams params;
    std::vector<anim::AnimatorRef> refs;
};

void buildWorld(World& w, const SharedAssets& a) {
    w.registry.init(4096);
    w.system.init(kAnimators);
    w.params.blend.resize(kAnimators);
    w.params.goWave.assign(kAnimators, 0);
    for (u32 i = 0; i < kAnimators; ++i) {
        w.params.blend[i] = static_cast<f32>(i % 11u) / 10.f;
    }
    for (u32 i = 0; i < kAnimators; ++i) {
        const ecs::EntityID e = w.registry.create();
        const anim::AnimatorRef ref = w.system.attach(w.registry, e, makeAnimator(a, w.params, i), a.skeleton);
        if (i % 3u == 0u) {
            anim::AnimatorInstance* inst = w.system.get(ref);
            inst->ik_enabled = true;
            inst->ik.root_bone = 5;
            inst->ik.mid_bone = 6;
            inst->ik.end_bone = 7;
            inst->ik.target = {0.3f + 0.001f * static_cast<f32>(i), 2.2f, 0.2f, 0.f};
            inst->ik.pole_vector = {0.f, 0.f, 1.f, 0.f};
        }
        w.refs.push_back(ref);
    }
}

/// Script for both worlds: transitions fire and blend weights move at fixed frames.
void drive(World& w, int frame) {
    for (u32 i = 0; i < kAnimators; ++i) {
        if (frame == 10 && i % 4u == 0u) {
            w.params.goWave[i] = 1;
        }
        if (frame == 50 && i % 8u == 0u) {
            w.params.goWave[i] = 0;
        }
        w.params.blend[i] = 0.5f + 0.5f * std::sin(0.05f * static_cast<f32>(frame) + 0.01f * static_cast<f32>(i));
    }
}

unsigned long long paletteHash(World& w) {
    unsigned long long h = 14695981039346656037ull;
    for (u32 i = 0; i < kAnimators; ++i) {
        const anim::AnimatorInstance* inst = w.system.get(w.refs[i]);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(inst->animator.bone_palette.data());
        const usize bytes = inst->animator.bone_palette.size() * sizeof(anim::mat4);
        for (usize b = 0; b < bytes; ++b) {
            h = (h ^ p[b]) * 1099511628211ull;
        }
    }
    return h;
}

bool sameOutputs(World& x, World& y) {
    for (u32 i = 0; i < kAnimators; ++i) {
        const anim::AnimatorInstance* a = x.system.get(x.refs[i]);
        const anim::AnimatorInstance* b = y.system.get(y.refs[i]);
        if (a == nullptr || b == nullptr || a->animator.bone_palette.size() != b->animator.bone_palette.size() ||
            a->animator.tick_count != b->animator.tick_count) {
            return false;
        }
        if (std::memcmp(a->animator.bone_palette.data(), b->animator.bone_palette.data(),
                        a->animator.bone_palette.size() * sizeof(anim::mat4)) != 0) {
            return false;
        }
    }
    return true;
}

void testParallelEqualsSerial() {
    SharedAssets assets;
    buildAssets(assets);
    World parallel;
    World serial;
    World oneWorker;
    buildWorld(parallel, assets);
    buildWorld(serial, assets);
    buildWorld(oneWorker, assets);

    fuse::jobs::JobScheduler& jobs = fuse::jobs::JobScheduler::instance();
    constexpr int kWarmup = 90; // covers both transitions and their crossfades
    constexpr int kFrames = 240;
    bool identical = true;
    bool workerCountIndependent = true;
    int firstMismatch = -1;
    std::vector<unsigned long long> frameHashes(kFrames, 0ull);

    // Pass 1: 3 workers (parallel) vs update_serial, frame by frame.
    jobs.setWorkerCount(3);
    g_allocations.store(0);
    for (int frame = 0; frame < kFrames; ++frame) {
        drive(parallel, frame);
        drive(serial, frame);
        g_counting.store(frame >= kWarmup);
        parallel.system.update(parallel.registry, kDt);
        g_counting.store(false);
        serial.system.update_serial(serial.registry, kDt);
        if (!sameOutputs(parallel, serial)) {
            identical = false;
            if (firstMismatch < 0) {
                firstMismatch = frame;
            }
        }
        frameHashes[static_cast<usize>(frame)] = paletteHash(parallel);
    }
    const u32 tasksWithThreeWorkers = parallel.system.stats().tasks;

    // Pass 2: the same script on 1 worker must reproduce every frame.
    jobs.setWorkerCount(1);
    for (int frame = 0; frame < kFrames; ++frame) {
        drive(oneWorker, frame);
        oneWorker.system.update(oneWorker.registry, kDt);
        if (paletteHash(oneWorker) != frameHashes[static_cast<usize>(frame)]) {
            workerCountIndependent = false;
        }
    }
    jobs.setWorkerCount(3);
    const unsigned long long allocations = g_allocations.load();
    std::printf("[gate] %u animators x %d frames: parallel tasks %u, first mismatch %d, steady-state allocations %llu\n",
                kAnimators, kFrames, tasksWithThreeWorkers, firstMismatch, allocations);
    expectTrue(parallel.system.stats().evaluated == kAnimators, "every Animator referenced from the registry evaluated");
    expectTrue(tasksWithThreeWorkers == 4u, "parallel update split into workerCount + 1 tasks");
    expectTrue(identical, "parallel == serial, bit-identical every frame");
    expectTrue(workerCountIndependent, "result independent of the worker count (3 vs 1 workers)");
    expectTrue(allocations == 0u, "zero heap allocations per steady-state parallel update");

    // The transitions really ran (crossfade into 'wave' and back).
    const anim::AnimatorInstance* inst = parallel.system.get(parallel.refs[8]);
    expectTrue(inst != nullptr && inst->animator.state_machine->active_state == 0u, "animator 8 returned to locomotion");
    const anim::AnimatorInstance* waving = parallel.system.get(parallel.refs[4]);
    expectTrue(waving != nullptr && waving->animator.state_machine->active_state == 1u, "animator 4 is waving");
    u32 ikSolved = 0;
    for (u32 i = 0; i < kAnimators; i += 3u) {
        ikSolved += parallel.system.get(parallel.refs[i])->ik_solved ? 1u : 0u;
    }
    expectTrue(ikSolved > 0u, "IK pass ran on the IK animators");

    // A destroyed slot is skipped (stale AnimatorRef) and a live one keeps working.
    parallel.system.destroy(parallel.refs[1]);
    parallel.system.update(parallel.registry, kDt);
    expectTrue(parallel.system.get(parallel.refs[1]) == nullptr, "destroyed animator resolves to null");
    expectTrue(parallel.system.stats().evaluated == kAnimators - 1u, "destroyed animator no longer evaluated");
}

} // namespace

int main() {
    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(3);
    testParallelEqualsSerial();
    fuse::jobs::JobScheduler::instance().shutdown();

    if (g_failures == 0) {
        std::printf("fuse animation system gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse animation system gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
