#pragma once

#include <fuse/types.hpp>

#include <complex>
#include <vector>

namespace fuse::audio {

/// Overlap-add FFT convolution reverb (CPU reference path and CI fallback).
class ConvReverbCpu {
public:
    void init(const float* ir_samples, u32 ir_length, u32 block_size);
    void reset();

    void process(const float* input, float* output, u32 frames);

    u32 fft_size() const { return m_fftSize; }
    u32 ir_length() const { return m_irLength; }

    /// Direct time-domain convolution for test reference.
    static void convolve_direct(const float* input, u32 input_len, const float* ir, u32 ir_len,
                                float* output);

    /// Peak error in dB between two buffers of equal length.
    static float peak_error_db(const float* a, const float* b, u32 length);

private:
    u32 m_fftSize = 0;
    u32 m_irLength = 0;
    u32 m_blockSize = 0;
    std::vector<std::complex<float>> m_irSpectrum;
    std::vector<float> m_overlap;
    std::vector<float> m_workInput;
    std::vector<float> m_workOutput;
};

} // namespace fuse::audio
