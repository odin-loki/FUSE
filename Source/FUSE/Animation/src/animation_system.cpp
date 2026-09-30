#include <fuse/animation/animation_system.hpp>

#include <fuse/animation/skinning.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/jobs/job_scheduler.hpp>

namespace fuse::animation {

namespace {

thread_local PoseScratchStack t_defaultPoseScratch;
thread_local PoseScratchStack* t_installedPoseScratch = nullptr;

} // namespace

PoseSoA& PoseScratchStack::push() {
    if (m_depth == m_poses.size()) {
        m_poses.push_back(std::make_unique<PoseSoA>());
    }
    return *m_poses[m_depth++];
}

void PoseScratchStack::pop() {
    if (m_depth > 0u) {
        --m_depth;
    }
}

std::vector<f32>& PoseScratchStack::push_weights() {
    if (m_weightDepth == m_weights.size()) {
        m_weights.push_back(std::make_unique<std::vector<f32>>());
    }
    return *m_weights[m_weightDepth++];
}

void PoseScratchStack::pop_weights() {
    if (m_weightDepth > 0u) {
        --m_weightDepth;
    }
}

PoseScratchStack& current_pose_scratch() {
    return t_installedPoseScratch != nullptr ? *t_installedPoseScratch : t_defaultPoseScratch;
}

ScopedPoseScratch::ScopedPoseScratch(PoseScratchStack& stack) : m_previous(t_installedPoseScratch) {
    t_installedPoseScratch = &stack;
}

ScopedPoseScratch::~ScopedPoseScratch() {
    t_installedPoseScratch = m_previous;
}

namespace {

Handle<AnimatorInstance> to_handle(AnimatorRef ref) {
    return Handle<AnimatorInstance>(ref.slot, ref.generation);
}

} // namespace

void AnimationSystem::init(u32 expected_animators) {
    shutdown();
    m_work.reserve(expected_animators);
}

void AnimationSystem::shutdown() {
    m_pool = HandleMap<AnimatorInstance>{};
    m_liveCount = 0;
    m_work.clear();
    m_scratch.clear();
    m_blendScratch.clear();
    m_stats = {};
}

AnimatorRef AnimationSystem::create(Animator&& animator, const Skeleton& skeleton) {
    AnimatorInstance instance;
    instance.animator = std::move(animator);
    instance.skeleton = &skeleton;
    // Size the pose / palette storage now so the first evaluation does not allocate.
    instance.animator.current_pose = Pose::make_bind_pose(skeleton);
    instance.animator.bone_palette.reserve(skeleton.bones.size());
    const Handle<AnimatorInstance> handle = m_pool.insert(std::move(instance));
    ++m_liveCount;
    if (m_work.capacity() < m_liveCount) {
        m_work.reserve(static_cast<usize>(m_liveCount) * 2u);
    }
    AnimatorRef ref;
    ref.slot = handle.index();
    ref.generation = handle.generation();
    return ref;
}

AnimatorRef AnimationSystem::attach(ecs::Registry& registry, ecs::EntityID entity, Animator&& animator,
                                    const Skeleton& skeleton) {
    const AnimatorRef ref = create(std::move(animator), skeleton);
    registry.add<AnimatorRef>(entity, ref);
    return ref;
}

void AnimationSystem::destroy(AnimatorRef ref) {
    if (!m_pool.valid(to_handle(ref))) {
        return;
    }
    m_pool.remove(to_handle(ref));
    --m_liveCount;
}

AnimatorInstance* AnimationSystem::get(AnimatorRef ref) {
    return m_pool.get(to_handle(ref));
}

const AnimatorInstance* AnimationSystem::get(AnimatorRef ref) const {
    return m_pool.get(to_handle(ref));
}

void AnimationSystem::evaluate(AnimatorInstance& instance, f32 dt, PoseSoA& scratch) {
    Animator& animator = instance.animator;
    if (animator.paused || instance.skeleton == nullptr) {
        return;
    }
    const Skeleton& skel = *instance.skeleton;
    const f32 scaled_dt = dt > 0.f ? dt * animator.playback_rate : 0.f;

    scratch.assign_bind_pose(skel);
    if (animator.state_machine) {
        animator.state_machine->evaluate_soa(scaled_dt, skel, scratch);
        ensure_pose_soa_bind_fallback(scratch, skel);
    }
    if (instance.ik_enabled) {
        instance.ik_solved = instance.ik.solve(scratch, skel);
    }
    scratch.to_pose(animator.current_pose);
    compute_skinning_palette(skel, animator.current_pose, animator.bone_palette);
    ++animator.tick_count;
}

void AnimationSystem::gather_(ecs::Registry& registry) {
    m_work.clear();
    registry.each<AnimatorRef>([this](ecs::EntityID, AnimatorRef& ref) {
        if (AnimatorInstance* instance = m_pool.get(to_handle(ref))) {
            m_work.push_back(instance);
        }
    });
}

void AnimationSystem::ensure_scratch_(u32 tasks) {
    if (m_scratch.size() < tasks) {
        m_scratch.resize(tasks);
    }
    if (m_blendScratch.size() < tasks) {
        m_blendScratch.resize(tasks);
    }
}

void AnimationSystem::update_serial(ecs::Registry& registry, f32 dt) {
    gather_(registry);
    ensure_scratch_(1u);
    ScopedPoseScratch scope(m_blendScratch[0]);
    for (AnimatorInstance* instance : m_work) {
        evaluate(*instance, dt, m_scratch[0]);
    }
    m_stats.evaluated = static_cast<u32>(m_work.size());
    m_stats.tasks = m_work.empty() ? 0u : 1u;
    ++m_stats.updates;
}

void AnimationSystem::update(ecs::Registry& registry, f32 dt, jobs::JobScheduler* scheduler) {
    jobs::JobScheduler& jobs = scheduler != nullptr ? *scheduler : jobs::JobScheduler::instance();
    if (!jobs.isInitialized() || jobs.isSingleThreaded()) {
        update_serial(registry, dt);
        return;
    }

    gather_(registry);
    const u32 count = static_cast<u32>(m_work.size());
    u32 tasks = jobs.workerCount() + 1u;
    if (tasks > count) {
        tasks = count;
    }
    ensure_scratch_(tasks);
    if (tasks > 0u) {
        // One contiguous range and one scratch pose per task; which thread runs a task does not
        // affect any result.
        jobs.parallel_for(0u, tasks, 1u, [this, count, tasks, dt](u32 task) {
            const u32 begin = static_cast<u32>((static_cast<u64>(count) * task) / tasks);
            const u32 end = static_cast<u32>((static_cast<u64>(count) * (task + 1u)) / tasks);
            PoseSoA& scratch = m_scratch[task];
            ScopedPoseScratch scope(m_blendScratch[task]);
            for (u32 i = begin; i < end; ++i) {
                evaluate(*m_work[i], dt, scratch);
            }
        });
    }
    m_stats.evaluated = count;
    m_stats.tasks = tasks;
    ++m_stats.updates;
}

} // namespace fuse::animation
