#include <fuse/renderer/cuda/stream_manager.hpp>

#include <fuse/jobs/cuda_jobs.hpp>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

namespace fuse::renderer::cuda {

void StreamManager::init() {
    if (m_initialized) {
        return;
    }
    m_initialized = true;
    m_available = fuse::jobs::cudaJobsAvailable();
#if defined(FUSE_HAS_CUDA)
    if (m_available) {
        for (usize i = 0; i < static_cast<usize>(CUDAStreamKind::Count); ++i) {
            cudaStream_t stream = nullptr;
            if (cudaStreamCreate(&stream) == cudaSuccess) {
                m_streams[i] = stream;
            } else {
                m_available = false;
                break;
            }
        }
    }
#endif
}

void StreamManager::shutdown() {
#if defined(FUSE_HAS_CUDA)
    for (usize i = 0; i < static_cast<usize>(CUDAStreamKind::Count); ++i) {
        if (m_streams[i] != nullptr) {
            cudaStreamDestroy(static_cast<cudaStream_t>(m_streams[i]));
            m_streams[i] = nullptr;
        }
    }
#endif
    m_available = false;
    m_initialized = false;
}

void* StreamManager::get(CUDAStreamKind stream) const {
    if (!m_available) {
        return nullptr;
    }
    const usize index = static_cast<usize>(stream);
    if (index >= static_cast<usize>(CUDAStreamKind::Count)) {
        return nullptr;
    }
    return m_streams[index];
}

} // namespace fuse::renderer::cuda
