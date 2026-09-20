#include <fuse/fx/particle_pool_gpu.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::fx {

namespace {

constexpr usize kBytesPerSlot = 40u; // pos(12) + vel(12) + lifetime(4) + age(4) + blend(4) + alive(4)

#if defined(FUSE_HAS_CUDA) && FUSE_HAS_CUDA
constexpr bool kCudaCompiled = true;

extern "C" void fuse_fx_particle_pool_cuda_stub(const u8* packed, u32 activeCount, float dt);
extern "C" u32 fuse_fx_particle_pool_device_ssbo_alloc_count();
extern "C" u32 fuse_fx_particle_pool_device_ssbo_reuse_count();
extern "C" u32 fuse_fx_particle_pool_device_ssbo_capacity_bytes();

void launchParticlePoolCudaStub(std::vector<u8>& packed, u32 activeCount, float dt) {
    fuse_fx_particle_pool_cuda_stub(packed.data(), activeCount, dt);
}
#else
constexpr bool kCudaCompiled = false;
#endif

} // namespace

ParticlePoolGpuBackend::ParticlePoolGpuBackend(u32 capacity)
    : m_capacity(capacity)
    , m_packed(capacity * kBytesPerSlot, 0)
    , m_cudaEnabled(kCudaCompiled) {}

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
        const float lifetime = *reinterpret_cast<const float*>(m_packed.data() + offset);
        offset += sizeof(float);
        const float age = *reinterpret_cast<const float*>(m_packed.data() + offset);
        offset += sizeof(float);
        offset += sizeof(float);
        const u32 aliveFlag = *reinterpret_cast<const u32*>(m_packed.data() + offset);
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

void ParticlePoolGpuBackend::cudaDispatchOrSkip(const frame::FrameCtx& ctx) {
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
    launchParticlePoolCudaStub(m_packed, m_activeCount, ctx.dt);
    m_deviceSsboCapacityBytes = fuse_fx_particle_pool_device_ssbo_capacity_bytes();
    m_deviceSsboAllocCount = fuse_fx_particle_pool_device_ssbo_alloc_count();
    m_deviceSsboReuseCount = fuse_fx_particle_pool_device_ssbo_reuse_count();
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
