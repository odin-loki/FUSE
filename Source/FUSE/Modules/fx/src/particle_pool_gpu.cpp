#include <fuse/fx/particle_pool_gpu.hpp>

#include <fuse/fx/particle_pool_kernel.hpp>
#include <fuse/jobs/cuda_jobs.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::fx {

namespace {

constexpr usize kBytesPerSlot = particle_pool_kernel::kBytesPerSlot; // pos vel lifetime age blend alive

#if defined(FUSE_HAS_CUDA) && FUSE_HAS_CUDA
constexpr bool kCudaCompiled = true;

extern "C" int fuse_fx_particle_pool_cuda_integrate(u8* packed, u32 slotCount, float dt, int hostDirty);
extern "C" u32 fuse_fx_particle_pool_device_ssbo_alloc_count();
extern "C" u32 fuse_fx_particle_pool_device_ssbo_reuse_count();
extern "C" u32 fuse_fx_particle_pool_device_ssbo_capacity_bytes();
extern "C" u32 fuse_fx_particle_pool_device_resident_frames();
extern "C" u32 fuse_fx_particle_pool_host_to_device_skip_count();

/// Integrates every packed slot (live slots may sit anywhere in the pool, not only below the active
/// count) with the single-source particle_pool_kernel::PackedKernel on the device.
bool launchParticlePoolCuda(std::vector<u8>& packed, float dt, bool hostDirty) {
    const u32 slotCount = static_cast<u32>(packed.size() / kBytesPerSlot);
    return fuse_fx_particle_pool_cuda_integrate(packed.data(), slotCount, dt, hostDirty ? 1 : 0) != 0;
}
#else
constexpr bool kCudaCompiled = false;
#endif

} // namespace

ParticlePoolGpuBackend::ParticlePoolGpuBackend(u32 capacity)
    : m_capacity(capacity)
    // Compiled-in CUDA is not enough: without a device (e.g. CUDA compile-only CI) the pool takes the
    // Disabled skip path instead of issuing cudaMalloc/kernel launches that can only fail.
    , m_cudaEnabled(kCudaCompiled && fuse::jobs::cudaJobsAvailable())
    , m_packed(capacity * kBytesPerSlot, 0) {}

void ParticlePoolGpuBackend::syncFromCpu(const ParticlePool& pool) {
    m_activeCount = pool.activeCount();
    const u32 count = std::min(m_capacity, static_cast<u32>(pool.slots().size()));
    if (m_packed.size() < count * kBytesPerSlot) {
        m_packed.resize(count * kBytesPerSlot, 0);
    }

    usize offset = 0;
    for (u32 slotIndex = 0; slotIndex < count; ++slotIndex) {
        const ParticleSlot& slot = pool.slots()[slotIndex];
        std::memcpy(m_packed.data() + offset, &slot.position.x, sizeof(float) * 3);
        offset += sizeof(float) * 3;
        std::memcpy(m_packed.data() + offset, &slot.velocity.x, sizeof(float) * 3);
        offset += sizeof(float) * 3;
        std::memcpy(m_packed.data() + offset, &slot.lifetime, sizeof(float));
        offset += sizeof(float);
        std::memcpy(m_packed.data() + offset, &slot.age, sizeof(float));
        offset += sizeof(float);
        std::memcpy(m_packed.data() + offset, &slot.blend_weight, sizeof(float));
        offset += sizeof(float);
        const u32 aliveFlag = slot.alive ? 1u : 0u;
        std::memcpy(m_packed.data() + offset, &aliveFlag, sizeof(u32));
        offset += sizeof(u32);
    }

    m_syncedFromCpu = true;
    m_hostDirty = true;
    ++m_syncCount;
}

void ParticlePoolGpuBackend::syncAliveFlagsToCpu(ParticlePool& pool) {
    if (!m_syncedFromCpu || m_packed.empty()) {
        return;
    }

    const u32 count = std::min(m_capacity, static_cast<u32>(pool.slots().size()));
    usize offset = 0;
    for (u32 slotIndex = 0; slotIndex < count; ++slotIndex) {
        offset += sizeof(float) * 6;
        offset += sizeof(float) * 2;
        offset += sizeof(float);
        const u32 aliveFlag = *reinterpret_cast<const u32*>(m_packed.data() + offset);
        offset += sizeof(u32);
        pool.setSlotAlive(slotIndex, aliveFlag != 0u);
    }

    ++m_writebackCount;
}

void ParticlePoolGpuBackend::syncPositionsToCpu(ParticlePool& pool) {
    if (!m_syncedFromCpu || m_packed.empty()) {
        return;
    }

    const u32 count = std::min(m_capacity, static_cast<u32>(pool.slots().size()));
    usize offset = 0;
    for (u32 slotIndex = 0; slotIndex < count; ++slotIndex) {
        float position[3];
        std::memcpy(position, m_packed.data() + offset, sizeof(float) * 3);
        offset += sizeof(float) * 3;
        float velocity[3];
        std::memcpy(velocity, m_packed.data() + offset, sizeof(float) * 3);
        offset += sizeof(float) * 3;
        pool.setSlotMotion(slotIndex, position[0], position[1], position[2], velocity[0], velocity[1],
                           velocity[2]);
        offset += sizeof(float) * 2;
        offset += sizeof(float);
        offset += sizeof(u32);
    }

    ++m_positionWritebackCount;
    ++m_writebackCount;
}

u32 ParticlePoolGpuBackend::syncSelectivePositionsToCpu(ParticlePool& pool) {
    if (!m_syncedFromCpu || m_packed.empty()) {
        return 0;
    }

    u32 written = 0;
    const u32 count = std::min(m_capacity, static_cast<u32>(pool.slots().size()));
    usize offset = 0;
    for (u32 slotIndex = 0; slotIndex < count; ++slotIndex) {
        float position[3];
        std::memcpy(position, m_packed.data() + offset, sizeof(float) * 3);
        offset += sizeof(float) * 3;
        float velocity[3];
        std::memcpy(velocity, m_packed.data() + offset, sizeof(float) * 3);
        offset += sizeof(float) * 3;
        offset += sizeof(float); // lifetime: not written back
        float age = 0.f;
        std::memcpy(&age, m_packed.data() + offset, sizeof(float));
        offset += sizeof(float);
        offset += sizeof(float);
        u32 aliveFlag = 0;
        std::memcpy(&aliveFlag, m_packed.data() + offset, sizeof(u32));
        offset += sizeof(u32);

        if (aliveFlag == 0u || age <= 0.f) {
            continue;
        }

        pool.setSlotMotion(slotIndex, position[0], position[1], position[2], velocity[0], velocity[1],
                           velocity[2]);
        ++written;
    }

    if (written > 0u) {
        ++m_selectiveWritebackCount;
        ++m_writebackCount;
    }
    return written;
}

void ParticlePoolGpuBackend::tick(const frame::FrameCtx& ctx) {
    cudaDispatchOrSkip(ctx);
}

void ParticlePoolGpuBackend::cudaDispatchOrSkip([[maybe_unused]] const frame::FrameCtx& ctx) {
    m_lastCudaSkipReason = ParticlePoolCudaSkipReason::None;

    if (!m_syncedFromCpu) {
        m_lastCudaSkipReason = ParticlePoolCudaSkipReason::NotSynced;
        ++m_cudaSkipCount;
        return;
    }

    syncResidencyFromPacked();

    if (!m_cudaEnabled) {
        m_lastCudaSkipReason = ParticlePoolCudaSkipReason::Disabled;
        ++m_cudaSkipCount;
        return;
    }
    if (m_packed.empty()) {
        m_lastCudaSkipReason = ParticlePoolCudaSkipReason::EmptyPool;
        ++m_cudaSkipCount;
        return;
    }
    if (m_activeCount == 0u) {
        m_lastCudaSkipReason = ParticlePoolCudaSkipReason::NoActiveParticles;
        ++m_cudaSkipCount;
        return;
    }

#if defined(FUSE_HAS_CUDA) && FUSE_HAS_CUDA
    const bool integrated = launchParticlePoolCuda(m_packed, ctx.dt, m_hostDirty);
    m_deviceSsboCapacityBytes = fuse_fx_particle_pool_device_ssbo_capacity_bytes();
    m_deviceSsboAllocCount = fuse_fx_particle_pool_device_ssbo_alloc_count();
    m_deviceSsboReuseCount = fuse_fx_particle_pool_device_ssbo_reuse_count();
    m_deviceResidentFrames = fuse_fx_particle_pool_device_resident_frames();
    m_hostToDeviceSkipCount = fuse_fx_particle_pool_host_to_device_skip_count();
    m_hostDirty = !integrated; // re-upload after a failed dispatch
#endif
    syncResidencyFromPacked();
    ++m_cudaDispatchCount;
}

u32 ParticlePoolGpuBackend::syncResidencyFromPacked() {
    if (!m_syncedFromCpu || m_packed.empty()) {
        m_residentSlotCount = 0;
        return 0;
    }

    u32 resident = 0;
    const u32 count = std::min(m_capacity, m_activeCount);
    usize offset = 0;
    for (u32 slotIndex = 0; slotIndex < count; ++slotIndex) {
        offset += sizeof(float) * 6;
        offset += sizeof(float) * 2;
        offset += sizeof(float);
        const u32 aliveFlag = *reinterpret_cast<const u32*>(m_packed.data() + offset);
        offset += sizeof(u32);
        if (aliveFlag != 0u) {
            ++resident;
        }
    }

    m_residentSlotCount = resident;
    ++m_residencySyncCount;
    return resident;
}

} // namespace fuse::fx
