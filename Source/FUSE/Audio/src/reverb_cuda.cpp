#include <fuse/audio/reverb_cuda.hpp>
#include <fuse/compute_kernel/stats.hpp>

namespace fuse::audio {

void ReverbCuda::init(const float* ir_samples, u32 ir_len, u32 block_size) {
    destroy();
    if (ir_samples == nullptr || ir_len == 0 || block_size == 0) {
        return;
    }
    m_blockSize = block_size;
    // Same partitioned FFT plan and single-source kernels as the CPU reverb. With a CUDA device the
    // plan is mirrored on it; otherwise every launch requests Backend::Cuda and kernel::launch falls
    // back to CpuParallel (recorded in the "reverb_fft" / "reverb_cmac" stats).
    m_cpuFallback.set_backend(kernel::Backend::Cuda);
    m_cpuFallback.init(ir_samples, ir_len, block_size);
    m_cudaAvailable = m_cpuFallback.enable_cuda();
    m_initialized = true;
}

void ReverbCuda::destroy() {
    m_initialized = false;
    m_cudaAvailable = false;
    m_blockSize = 0;
    m_cpuFallback = ConvReverbCpu{};
}

void ReverbCuda::process(const float* input, float* output, u32 frames) {
    if (!m_initialized) {
        for (u32 i = 0; i < frames; ++i) {
            output[i] = input[i];
        }
        return;
    }
    m_cpuFallback.process(input, output, frames);
}

} // namespace fuse::audio
