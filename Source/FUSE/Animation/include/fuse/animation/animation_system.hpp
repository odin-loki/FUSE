#pragma once

// MP-B7.1-PARALLEL-EVAL / GAP-GAME-LOOP-ECS: the ECS animation system.
//
// Animator owns heap state (state machine, pose, palette), so it cannot be an ECS column (columns
// are trivially destructible bytes). The system keeps the Animators in a generation-checked pool
// and entities carry an `AnimatorRef` component that names their pool slot. `update` evaluates every
// Animator referenced from the registry — state machine / blend tree, optional two-bone IK, forward
// kinematics and the skinning palette — in parallel over JobScheduler::parallel_for.
//
// Parallel evaluation: the referenced slots are gathered in registry iteration order, split into
// one contiguous range per task (at most workerCount + 1 tasks), and each task evaluates its range
// into its own scratch PoseSoA. An Animator's result depends only on its own state and dt, so the
// output is bit-identical to `update_serial` and independent of the worker count. Steady state (no
// Animator added / removed and poses already at their bone counts) makes no heap allocations.
//
// Thread-safety contract for users: state-machine transition conditions and enter/exit callbacks
// run on worker threads and must only touch their own Animator's state; clips and skeletons are
// shared read-only.

#include <fuse/animation/animator.hpp>
#include <fuse/animation/ik_solver.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/handle.hpp>
#include <fuse/handle_map.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <vector>

namespace fuse::ecs {
class Registry;
}

namespace fuse::jobs {
class JobScheduler;
}

namespace fuse::animation {

/// LIFO pool of scratch poses the blend nodes (BlendNode2, blend spaces, layered / additive, state
/// machine crossfades) borrow while evaluating. Once every depth has grown to the skeleton size the
/// pool never allocates. Poses live behind unique_ptr so references stay valid while it grows.
class PoseScratchStack {
public:
    PoseSoA& push();
    void pop();
    /// Weight buffers (blend-space sampling), same LIFO discipline.
    std::vector<f32>& push_weights();
    void pop_weights();
    [[nodiscard]] usize depth() const { return m_depth; }
    [[nodiscard]] usize pooled() const { return m_poses.size(); }

private:
    std::vector<std::unique_ptr<PoseSoA>> m_poses;
    usize m_depth = 0;
    std::vector<std::unique_ptr<std::vector<f32>>> m_weights;
    usize m_weightDepth = 0;
};

/// The blend-node scratch pool of the calling thread: the one installed by a ScopedPoseScratch,
/// else a thread-local default. AnimationSystem installs one pool per parallel task, so which
/// worker runs a task never changes where (or whether) scratch memory is allocated.
PoseScratchStack& current_pose_scratch();

/// Installs `stack` as the calling thread's blend-node scratch pool for this scope.
class ScopedPoseScratch {
public:
    explicit ScopedPoseScratch(PoseScratchStack& stack);
    ~ScopedPoseScratch();
    ScopedPoseScratch(const ScopedPoseScratch&) = delete;
    ScopedPoseScratch& operator=(const ScopedPoseScratch&) = delete;

private:
    PoseScratchStack* m_previous = nullptr;
};

/// ECS component: the entity's Animator slot in an AnimationSystem.
struct AnimatorRef {
    static constexpr const char* component_name = "AnimatorRef";

    u32 slot = 0xFFFFFFFFu;
    u32 generation = 0;

    [[nodiscard]] bool valid() const { return slot != 0xFFFFFFFFu; }
};

/// Pool entry: the Animator plus what the system needs to evaluate it.
struct AnimatorInstance {
    Animator animator;
    const Skeleton* skeleton = nullptr; ///< not owned; must outlive the instance
    /// Optional two-bone IK pass applied to the evaluated local pose before skinning.
    bool ik_enabled = false;
    TwoBoneIK ik{};
    /// Last IK solve succeeded.
    bool ik_solved = false;
};

struct AnimationSystemStats {
    u32 evaluated = 0; ///< Animators evaluated by the last update
    u32 tasks = 0;     ///< parallel tasks (scratch poses) used by the last update
    u64 updates = 0;
};

class AnimationSystem {
public:
    /// `expected_animators` pre-sizes the gather list.
    void init(u32 expected_animators = 256);
    void shutdown();

    /// Adds an Animator to the pool (structural: may allocate).
    AnimatorRef create(Animator&& animator, const Skeleton& skeleton);
    /// create + registry.add<AnimatorRef>(entity).
    AnimatorRef attach(ecs::Registry& registry, ecs::EntityID entity, Animator&& animator, const Skeleton& skeleton);
    /// Releases the slot (a stale AnimatorRef then resolves to nullptr).
    void destroy(AnimatorRef ref);
    AnimatorInstance* get(AnimatorRef ref);
    const AnimatorInstance* get(AnimatorRef ref) const;
    u32 animator_count() const { return m_liveCount; }

    /// Evaluates every Animator referenced by an AnimatorRef in `registry` (parallel when the
    /// scheduler is initialised with workers; `scheduler` null = JobScheduler::instance()).
    void update(ecs::Registry& registry, f32 dt, jobs::JobScheduler* scheduler = nullptr);
    /// Same evaluation on the calling thread only (reference for the parallel path).
    void update_serial(ecs::Registry& registry, f32 dt);

    const AnimationSystemStats& stats() const { return m_stats; }

    /// One Animator's evaluation (what both update paths run per instance).
    static void evaluate(AnimatorInstance& instance, f32 dt, PoseSoA& scratch);

private:
    void gather_(ecs::Registry& registry);
    void ensure_scratch_(u32 tasks);

    HandleMap<AnimatorInstance> m_pool;
    u32 m_liveCount = 0;
    std::vector<AnimatorInstance*> m_work;
    std::vector<PoseSoA> m_scratch;
    std::vector<PoseScratchStack> m_blendScratch;
    AnimationSystemStats m_stats{};
};

} // namespace fuse::animation
