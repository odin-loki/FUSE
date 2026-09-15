#pragma once

#include <fuse/types.hpp>

namespace fuse::audio {

/// CUDA convolution reverb facade — uses GPU when compiled with FUSE_AUDIO_CUDA, else CPU fallback.
class ReverbCuda {
public:
    bool available() const { return m_cudaAvailable; }

    void init(const float* ir_samples, u32 ir_len, u32 block_size);
    void destroy();

    void process(const float* input, float* output, u32 frames);

private:
    bool m_cudaAvailable = false;
    bool m_initialized = false;
    u32 m_blockSize = 0;
};

} // namespace fuse::audio
