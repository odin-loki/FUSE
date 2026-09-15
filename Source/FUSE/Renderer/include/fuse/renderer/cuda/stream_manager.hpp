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

    bool available() const { return m_available; }

private:
    bool m_initialized = false;
    bool m_available = false;
    void* m_streams[static_cast<usize>(CUDAStreamKind::Count)]{};
};

} // namespace fuse::renderer::cuda
