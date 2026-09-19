#pragma once

// Ore: Engine/source/afx/util/afxParticlePool.h GPU backend stub (packed SSBO mirror without CUDA)

#include <fuse/fx/particle_pool.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::fx {

/// Packed GPU-ready mirror of CPU ParticlePool slots (B7.7 layout stub).
class ParticlePoolGpuBackend {
public:
    explicit ParticlePoolGpuBackend(u32 capacity = 64);

    u32 capacity() const { return m_capacity; }
    u32 activeCount() const { return m_activeCount; }
    u32 packedDeviceBytes() const { return static_cast<u32>(m_packed.size()); }
    bool hasDeviceBinding() const { return !m_packed.empty(); }
    u32 syncCount() const { return m_syncCount; }

    void syncFromCpu(const ParticlePool& pool);
    void tick(const frame::FrameCtx& ctx);

    const std::vector<u8>& packed() const { return m_packed; }

private:
    u32 m_capacity = 0;
    u32 m_activeCount = 0;
    u32 m_syncCount = 0;
    std::vector<u8> m_packed;
};

} // namespace fuse::fx
