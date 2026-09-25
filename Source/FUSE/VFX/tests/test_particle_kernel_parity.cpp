// Gate for the single-source particle port (fuse/vfx/particle_sim_kernel.hpp):
//   - simulate_step / lifetime_cull on CpuReference and CpuParallel (0/2/4 workers) leave bit-identical
//     SoA columns, counts, free lists and dead-slot lists over many steps with emission, colliders and
//     deaths spread over every workgroup (capacity not a multiple of the 256-slot workgroup).
//   - The compaction is count -> scan -> write: dead lists are strictly descending and contain exactly
//     the slots that died, whatever the worker count.
//   - Launches record "particle_update" / "particle_compact" stats; Cuda / Auto / VulkanCompute without
//     a device fall back to CpuParallel (recorded) with the same state; particle_cuda_kernel_available()
//     and ParticleSystem::backend_kind() report that; steady-state steps reuse every buffer.

#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/vfx/particle_sim_kernel.hpp>
#include <fuse/vfx/particle_soa_ops.hpp>
#include <fuse/vfx/particle_system.hpp>

#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::usize;
namespace vfx = fuse::vfx;
namespace soa_ops = fuse::vfx::particle_soa;
namespace psk = fuse::vfx::particle_sim_kernel;
namespace kernel = fuse::kernel;
using fuse::math::Vec3;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kCapacity = 5000; // 19 full 256-slot workgroups + a partial edge workgroup

vfx::ParticleEmitterDesc makeDesc() {
    vfx::ParticleEmitterDesc desc{};
    desc.max_particles = kCapacity;
    desc.lifetime_min = 0.2f;
    desc.lifetime_max = 1.4f;
    desc.velocity_min = {-3.f, -2.f, -3.f};
    desc.velocity_max = {3.f, 6.f, 3.f};
    desc.position_spread = {2.f, 0.5f, 2.f};
    desc.drag = 0.3f;
    desc.collide_with_world = true;
    vfx::ParticleCollider ground{};
    ground.shape = vfx::ParticleColliderShape::Plane;
    ground.normal = {0.f, 1.f, 0.f};
    ground.offset = -0.5f;
    vfx::ParticleCollider ball{};
    ball.shape = vfx::ParticleColliderShape::Sphere;
    ball.center = {0.5f, 0.2f, -0.3f};
    ball.radius = 0.8f;
    desc.colliders = {ground, ball};
    desc.restitution = 0.6f;
    desc.friction = 0.2f;
    return desc;
}

struct StepLog {
    std::vector<u32> dead;       ///< Concatenated dead lists of every step.
    std::vector<u32> counts;     ///< alive_after / culled / integrated per step.
    bool descending = true;      ///< Every per-step dead list strictly descending.
    bool dead_match_flags = true; ///< Dead list == slots whose alive flag went 1 -> 0.
};

template <typename T>
kernel::ParityReport compareVec(const std::vector<T>& a, const std::vector<T>& b) {
    return kernel::compare_bitwise(std::span<const T>(a), std::span<const T>(b));
}

kernel::ParityReport compareSoa(const vfx::ParticleSoA& a, const vfx::ParticleSoA& b) {
    kernel::ParityReport r = compareVec(a.positions, b.positions);
    r.merge(compareVec(a.velocities, b.velocities));
    r.merge(compareVec(a.ages, b.ages));
    r.merge(compareVec(a.lifetimes, b.lifetimes));
    r.merge(compareVec(a.sizes, b.sizes));
    r.merge(compareVec(a.colors, b.colors));
    r.merge(compareVec(a.alphas, b.alphas));
    r.merge(compareVec(a.alive_flags, b.alive_flags));
    r.merge(compareVec(a.free_slots, b.free_slots));
    if (a.count != b.count) {
        r.ok = false;
        ++r.mismatches;
    }
    return r;
}

/// Burst, then `steps` simulation steps with re-emission every third step (and a lifetime-only cull
/// every fifth), all on `backend`.
vfx::ParticleSoA runScenario(kernel::Backend backend, u32 steps, StepLog& log) {
    const vfx::ParticleEmitterDesc desc = makeDesc();
    vfx::ParticleSoA soa{};
    soa_ops::init(soa, kCapacity);
    u64 seed = 7;
    seed = soa_ops::burst_emit(soa, desc, {0.f, 1.f, 0.f}, 4200, seed).seed_after;
    std::vector<u32> dead;
    std::vector<u32> before;
    for (u32 step = 0; step < steps; ++step) {
        before = soa.alive_flags;
        u32 culled = 0;
        if (step % 5u == 4u) {
            const soa_ops::LifetimeCullResult r = soa_ops::lifetime_cull_on(backend, soa, 0.05f);
            dead = r.dead_slots;
            culled = r.culled;
            log.counts.insert(log.counts.end(), {r.alive_after, r.culled, r.aged});
        } else {
            const soa_ops::SimStepResult r = soa_ops::simulate_step_on(backend, soa, desc, 1.f / 30.f, dead);
            culled = r.culled;
            log.counts.insert(log.counts.end(), {r.alive_after, r.culled, r.integrated});
        }
        log.dead_match_flags = log.dead_match_flags && dead.size() == culled;
        for (usize i = 0; i < dead.size(); ++i) {
            log.descending = log.descending && (i == 0 || dead[i - 1] > dead[i]);
            log.dead_match_flags =
                log.dead_match_flags && before[dead[i]] == 1u && soa.alive_flags[dead[i]] == 0u;
        }
        u32 died = 0;
        for (u32 i = 0; i < kCapacity; ++i) {
            died += (before[i] == 1u && soa.alive_flags[i] == 0u) ? 1u : 0u;
            log.dead_match_flags = log.dead_match_flags && soa.alive_flags[i] <= 1u;
        }
        log.dead_match_flags = log.dead_match_flags && died == culled && soa.count == soa_ops::count_live_flags(soa);
        log.dead.insert(log.dead.end(), dead.begin(), dead.end());
        if (step % 3u == 2u) {
            seed = soa_ops::burst_emit(soa, desc, {0.2f, 1.5f, -0.1f}, 700, seed).seed_after;
        }
    }
    return soa;
}

void testBackendParity() {
    constexpr u32 kSteps = 45;
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    StepLog refLog{};
    const vfx::ParticleSoA reference = runScenario(kernel::Backend::CpuReference, kSteps, refLog);
    expectTrue(refLog.descending, "reference dead lists strictly descending");
    expectTrue(refLog.dead_match_flags, "reference dead lists == slots that died; counts consistent");
    u32 totalDead = static_cast<u32>(refLog.dead.size());
    expectTrue(totalDead > kCapacity, "scenario kills particles in every workgroup many times over");

    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        StepLog log{};
        const vfx::ParticleSoA parallel = runScenario(kernel::Backend::CpuParallel, kSteps, log);
        kernel::ParityReport report = compareSoa(reference, parallel);
        report.merge(compareVec(refLog.dead, log.dead));
        report.merge(compareVec(refLog.counts, log.counts));
        char label[160];
        std::snprintf(label, sizeof(label),
                      "CpuReference == CpuParallel bit-exact particles (%u workers, %llu mismatches)", workers,
                      static_cast<unsigned long long>(report.mismatches));
        expectTrue(report.ok, label);
        expectTrue(log.descending && log.dead_match_flags, "parallel dead lists descending + exact");
    }

    // The default (heuristic) entry points pick CpuReference below the parallel threshold and match.
    expectTrue(soa_ops::cpu_simulation_backend(kCapacity) == kernel::Backend::CpuReference &&
                   soa_ops::cpu_simulation_backend(soa_ops::kParallelSlotThreshold) == kernel::Backend::CpuParallel,
               "cpu_simulation_backend threshold");
    scheduler.shutdown();
}

void testStatsFallbackAndSteadyState() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    const vfx::ParticleEmitterDesc desc = makeDesc();
    kernel::reset_kernel_stats();

    // Every particle has the same lifetime: they all die in the same step across all workgroups.
    vfx::ParticleEmitterDesc uniform = desc;
    uniform.lifetime_min = uniform.lifetime_max = 0.1f;
    vfx::ParticleSoA soa{};
    soa_ops::init(soa, kCapacity);
    (void)soa_ops::burst_emit(soa, uniform, {}, kCapacity, 3);
    std::vector<u32> dead;
    const soa_ops::SimStepResult survive = soa_ops::simulate_step_on(kernel::Backend::CpuReference, soa, uniform, 0.05f, dead);
    expectTrue(survive.culled == 0u && survive.integrated == kCapacity && dead.empty(), "first step: no deaths");
    kernel::KernelStats update{};
    kernel::KernelStats compact{};
    expectTrue(kernel::find_kernel_stats(psk::kUpdateName, update) && update.launches == 1u &&
                   update.items == kCapacity && update.workgroups == 20u &&
                   update.last_backend == kernel::Backend::CpuReference,
               "particle_update stats (items, 256-wide workgroups, backend)");
    expectTrue(!kernel::find_kernel_stats(psk::kCompactName, compact), "no compaction launch without deaths");

    const soa_ops::SimStepResult allDie = soa_ops::simulate_step_on(kernel::Backend::CpuParallel, soa, uniform, 0.06f, dead);
    bool fullDescending = allDie.culled == kCapacity && dead.size() == kCapacity && soa.count == 0u;
    for (u32 i = 0; fullDescending && i < kCapacity; ++i) {
        fullDescending = dead[i] == kCapacity - 1u - i;
    }
    expectTrue(fullDescending, "all slots dying at once compact to kCapacity-1 .. 0");
    expectTrue(kernel::find_kernel_stats(psk::kCompactName, compact) && compact.launches == 1u &&
                   compact.items == kCapacity && compact.last_backend == kernel::Backend::CpuParallel,
               "particle_compact stats");
    expectTrue(soa_ops::allocate_slot(soa) == 0u, "free list hands out the lowest slot first");

    const bool device = kernel::backend_available(kernel::Backend::Cuda);
    expectTrue(vfx::particle_cuda_kernel_available() == device,
               "particle_cuda_kernel_available() == compiled kernel && device present");
    {
        vfx::ParticleSystem system{};
        vfx::VfxDesc systemDesc{};
        systemDesc.gpu_simulation = true;
        system.init(systemDesc);
        expectTrue(system.backend_kind() == (device ? vfx::VfxBackendKind::Cuda : vfx::VfxBackendKind::CpuReference),
                   "ParticleSystem backend_kind reflects the particle kernel + device");
        const fuse::Handle<vfx::ParticleEmitter> h = system.create_emitter(desc);
        const vfx::ParticleEmitter* emitter = system.get_emitter(h);
        expectTrue(emitter != nullptr && emitter->gpu_simulation() == device, "emitter simulates where reported");
        system.destroy();
    }

    StepLog refLog{};
    const vfx::ParticleSoA reference = runScenario(kernel::Backend::CpuReference, 12, refLog);
    if (!device) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            StepLog log{};
            const vfx::ParticleSoA fallback = runScenario(gpu, 12, log);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel,
                       "GPU particle request without a device falls back to CpuParallel (recorded)");
            kernel::ParityReport report = compareSoa(reference, fallback);
            report.merge(compareVec(refLog.dead, log.dead));
            expectTrue(report.ok, "fallback particles == CpuReference");
        }
    } else {
        StepLog log{};
        const vfx::ParticleSoA gpu = runScenario(kernel::Backend::Cuda, 1, log);
        StepLog oneLog{};
        const vfx::ParticleSoA one = runScenario(kernel::Backend::CpuReference, 1, oneLog);
        expectTrue(compareVec(one.alive_flags, gpu.alive_flags).ok && compareVec(oneLog.dead, log.dead).ok,
                   "CUDA particle step: same deaths and dead-slot order as CpuReference");
        expectTrue(kernel::compare_floats(std::span<const Vec3>(one.positions), std::span<const Vec3>(gpu.positions),
                                          {1e-4, 1e-5})
                       .ok,
                   "CUDA particle positions within tolerance");

        // 5,000 particles leave most of the 3090 idle (achieved occupancy ~16%). A full grid is
        // what the >70% Nsight gate measures.
        constexpr u32 kFill = 262144u;
        vfx::ParticleSoA fill{};
        soa_ops::init(fill, kFill);
        vfx::ParticleEmitterDesc fillDesc = desc;
        fillDesc.max_particles = kFill;
        (void)soa_ops::burst_emit(fill, fillDesc, {}, kFill, 1);
        std::vector<u32> deadFill;
        const soa_ops::SimStepResult filled =
            soa_ops::simulate_step_on(kernel::Backend::Cuda, fill, fillDesc, 1.f / 60.f, deadFill);
        expectTrue(filled.integrated == kFill, "CUDA fill launch of 262144 particles");
    }

    // Steady state: stepping reuses the dead-slot scratch, the kernel scratch and the free list.
    vfx::ParticleSoA steady{};
    soa_ops::init(steady, kCapacity);
    u64 seed = 11;
    seed = soa_ops::burst_emit(steady, desc, {}, 3000, seed).seed_after;
    std::vector<u32> scratch;
    (void)soa_ops::simulate_step(steady, desc, 1.f / 30.f, scratch);
    const u32* scratchData = scratch.data();
    const u32* simScratch = steady.sim_scratch.data();
    const u32* freeData = steady.free_slots.data();
    for (int frame = 0; frame < 60; ++frame) {
        seed = soa_ops::burst_emit(steady, desc, {}, 60, seed).seed_after;
        (void)soa_ops::simulate_step(steady, desc, 1.f / 30.f, scratch);
    }
    expectTrue(scratch.data() == scratchData && steady.sim_scratch.data() == simScratch &&
                   steady.free_slots.data() == freeData,
               "steady-state simulate_step reuses every buffer");

    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParity();
    testStatsFallbackAndSteadyState();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d particle kernel parity check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("Particle kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
