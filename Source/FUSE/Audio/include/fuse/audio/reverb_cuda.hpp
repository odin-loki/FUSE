#pragma once

#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

/// CUDA convolution reverb facade — uses GPU when compiled with FUSE_AUDIO_CUDA, else the CPU
/// overlap-add path (\ref ConvReverbCpu) so output is identical in headless / non-CUDA builds.
class ReverbCuda {
public:
    bool available() const { return m_cudaAvailable; }
    bool initialized() const { return m_initialized; }

    void init(const float* ir_samples, u32 ir_len, u32 block_size);
    void destroy();

    /// Convolve one block. Uninitialised reverbs pass input through unchanged.
    void process(const float* input, float* output, u32 frames);

private:
    bool m_cudaAvailable = false;
    bool m_initialized = false;
    u32 m_blockSize = 0;
    ConvReverbCpu m_cpuFallback;
};

} // namespace fuse::audio
