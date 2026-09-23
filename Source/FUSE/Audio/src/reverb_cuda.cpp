#include <fuse/audio/reverb_cuda.hpp>

namespace fuse::audio {

void ReverbCuda::init(const float* ir_samples, u32 ir_len, u32 block_size) {
    destroy();
    if (ir_samples == nullptr || ir_len == 0 || block_size == 0) {
        return;
    }
    m_blockSize = block_size;
    // No GPU FFT backend is compiled into fuse_audio yet; the CPU overlap-add path is the
    // reference implementation the CUDA kernel must match (see test_b7_audio_gates).
    m_cudaAvailable = false;
    m_cpuFallback.init(ir_samples, ir_len, block_size);
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
