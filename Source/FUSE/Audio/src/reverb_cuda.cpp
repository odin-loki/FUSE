#include <fuse/audio/reverb_cuda.hpp>

#include <fuse/audio/conv_reverb_cpu.hpp>

#include <vector>

namespace fuse::audio {

void ReverbCuda::init(const float* ir_samples, u32 ir_len, u32 block_size) {
    destroy();
    m_blockSize = block_size;
    m_initialized = true;
    m_cudaAvailable = false;
    (void)ir_samples;
    (void)ir_len;
}

void ReverbCuda::destroy() {
    m_initialized = false;
    m_cudaAvailable = false;
    m_blockSize = 0;
}

void ReverbCuda::process(const float* input, float* output, u32 frames) {
    if (!m_initialized) {
        for (u32 i = 0; i < frames; ++i) {
            output[i] = input[i];
        }
        return;
    }
    for (u32 i = 0; i < frames; ++i) {
        output[i] = input[i];
    }
}

} // namespace fuse::audio
