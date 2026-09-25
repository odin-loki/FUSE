// Gate for the single-source AFX particle pool integrate (fuse/fx/particle_pool_kernel.hpp):
//   - ParticlePool::tick (SlotKernel) and the packed-SSBO PackedKernel that CUDA runs produce the same
//     bits for positions, ages and alive flags over many frames (sparse live slots, expiries).
//   - PackedKernel on CpuReference == CpuParallel (0/2/4 workers), partial edge workgroup.
//   - Launch stats carry the kernel names; a Cuda request without a device falls back to CpuParallel.

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/fx/particle_pool.hpp>
#include <fuse/fx/particle_pool_gpu.hpp>
#include <fuse/fx/particle_pool_kernel.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u8;
namespace fx = fuse::fx;
namespace ppk = fuse::fx::particle_pool_kernel;
namespace kernel = fuse::kernel;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kSlots = 150; // two full 64-wide workgroups + a partial one

fx::ParticlePool makePool() {
    fx::ParticlePool pool(kSlots);
    for (u32 i = 0; i < kSlots; ++i) {
        const f32 fi = static_cast<f32>(i);
        pool.spawn({fi * 0.1f, 1.f, -fi * 0.05f}, {0.3f + fi * 0.01f, -0.7f, fi * 0.02f}, 0.05f + 0.013f * fi);
    }
    // Sparse live slots: kill every third one so live slots are not a prefix of the pool.
    for (u32 i = 0; i < kSlots; i += 3) {
        pool.setSlotAlive(i, false);
    }
    return pool;
}

/// Same layout ParticlePoolGpuBackend::syncFromCpu writes.
std::vector<u8> pack(const fx::ParticlePool& pool) {
    fx::ParticlePoolGpuBackend backend(pool.capacity());
    backend.syncFromCpu(pool);
    return backend.packed();
}

struct Unpacked {
    std::vector<f32> floats; ///< px py pz vx vy vz lifetime age per slot
    std::vector<u32> alive;
};

Unpacked unpack(const std::vector<u8>& packed) {
    Unpacked u;
    const u32 n = static_cast<u32>(packed.size() / ppk::kBytesPerSlot);
    for (u32 i = 0; i < n; ++i) {
        f32 f[8];
        u32 alive = 0;
        std::memcpy(f, packed.data() + i * ppk::kBytesPerSlot, sizeof(f));
        std::memcpy(&alive, packed.data() + i * ppk::kBytesPerSlot + ppk::kAliveOffset, sizeof(alive));
        u.floats.insert(u.floats.end(), f, f + 8);
        u.alive.push_back(alive);
    }
    return u;
}

bool runPacked(kernel::Backend backend, std::vector<u8>& packed, f32 dt) {
    ppk::PackedParams params{};
    params.packed = {packed.data(), static_cast<u32>(packed.size())};
    params.dt = ppk::resolve_dt(dt);
    return kernel::launch(backend, ppk::packed_launch(static_cast<u32>(packed.size() / ppk::kBytesPerSlot)),
                          ppk::PackedKernel{}, params)
        .ok;
}

void testPoolVsPackedParity() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    const u32 initialActive = makePool().activeCount();
    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        fx::ParticlePool frames = makePool();
        std::vector<u8> serial = pack(frames);
        std::vector<u8> parallel = serial;
        bool same = true;
        for (u32 frame = 0; frame < 40; ++frame) {
            fuse::frame::FrameCtx ctx{};
            ctx.dt = frame % 7u == 3u ? 0.f : 1.f / 60.f; // dt <= 0 resolves to 1/60 on every path
            frames.tick(ctx);
            same = same && runPacked(kernel::Backend::CpuReference, serial, ctx.dt) &&
                   runPacked(kernel::Backend::CpuParallel, parallel, ctx.dt);
            const Unpacked a = unpack(pack(frames));
            const Unpacked b = unpack(serial);
            const Unpacked c = unpack(parallel);
            same = same && kernel::compare_bitwise(std::span<const f32>(a.floats), std::span<const f32>(b.floats)).ok &&
                   kernel::compare_bitwise(std::span<const u32>(a.alive), std::span<const u32>(b.alive)).ok &&
                   kernel::compare_bitwise(std::span<const f32>(b.floats), std::span<const f32>(c.floats)).ok &&
                   kernel::compare_bitwise(std::span<const u32>(b.alive), std::span<const u32>(c.alive)).ok;
        }
        char label[128];
        std::snprintf(label, sizeof(label), "pool tick == packed kernel (ref + parallel) bit-exact, %u workers", workers);
        expectTrue(same, label);
        u32 live = 0;
        for (const fx::ParticleSlot& s : frames.slots()) {
            live += s.alive ? 1u : 0u;
        }
        expectTrue(frames.activeCount() == live && live < initialActive && live > 0u,
                   "pool active count tracks expiries");
    }
    scheduler.shutdown();
}

void testStatsAndFallback() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    kernel::reset_kernel_stats();
    fx::ParticlePool pool = makePool();
    fuse::frame::FrameCtx ctx{};
    ctx.dt = 1.f / 30.f;
    pool.tick(ctx);
    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(ppk::kSlotName, stats) && stats.launches == 1u && stats.items == kSlots &&
                   stats.workgroups == 3u && stats.last_backend == kernel::Backend::CpuReference,
               "ParticlePool::tick records fx_particle_pool_integrate stats");

    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        std::vector<u8> reference = pack(makePool());
        std::vector<u8> fallback = reference;
        expectTrue(runPacked(kernel::Backend::CpuReference, reference, ctx.dt), "reference packed launch");
        expectTrue(runPacked(kernel::Backend::Cuda, fallback, ctx.dt), "Cuda packed launch falls back");
        const kernel::LaunchRecord last = kernel::last_launch();
        expectTrue(last.ok && last.requested == kernel::Backend::Cuda && last.backend == kernel::Backend::CpuParallel &&
                       std::strcmp(last.name, ppk::kPackedName) == 0,
                   "GPU packed request without a device falls back to CpuParallel (recorded)");
        expectTrue(reference == fallback, "fallback packed result == CpuReference");
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testPoolVsPackedParity();
    testStatsAndFallback();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d fx particle pool kernel check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("FX particle pool kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
