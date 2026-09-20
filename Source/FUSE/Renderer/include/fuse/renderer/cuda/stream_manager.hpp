#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer::cuda {

enum class CUDAStreamKind : u8 {
    Render,
    Physics,
    AI,
    Particles,
    Upload,
    Count
};

/// Named CUDA streams for workload separation (stub until full B2.6 wiring).
class StreamManager {
public:
    void init();
    void shutdown();

    /// Opaque `cudaStream_t` when available; otherwise null.
    void* get(CUDAStreamKind stream) const;

    /// cudaStreamSynchronize when available; returns false if stream missing / no CUDA.
    bool synchronize(CUDAStreamKind stream) const;
    /// Synchronize all created streams. Returns true if every non-null stream synced (or none exist and !available).
    bool synchronizeAll() const;
    /// Count of non-null `m_streams` (0 on stub / before init).
    u32 createdStreamCount() const;

    bool available() const { return m_available; }

private:
    bool m_initialized = false;
    bool m_available = false;
    void* m_streams[static_cast<usize>(CUDAStreamKind::Count)]{};
};

} // namespace fuse::renderer::cuda
