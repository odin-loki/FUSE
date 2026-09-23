#pragma once

#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

/// CUDA convolution reverb facade over the partitioned FFT plan of \ref ConvReverbCpu: the same
/// single-source "reverb_fft" / "reverb_cmac" kernels run on a CUDA device mirror when a device is
/// present, else each launch requests Backend::Cuda and falls back to CpuParallel (recorded in the
/// kernel stats), so output matches the CPU reverb in headless / non-CUDA builds.
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
