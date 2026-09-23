#include <fuse/audio/conv_reverb_cpu.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace fuse::audio {

namespace {

u32 next_pow2(u32 value) {
    u32 result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

/// Radix-2 DIT FFT using a precomputed twiddle table (w_k = exp(-2*pi*i*k/n), k < n/2).
/// Twiddles are evaluated in double precision so error does not accumulate with size.
void fft_inplace(std::vector<std::complex<float>>& data,
                 const std::vector<std::complex<float>>& twiddles) {
    const u32 n = static_cast<u32>(data.size());
    for (u32 i = 1, j = 0; i < n; ++i) {
        u32 bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    for (u32 len = 2; len <= n; len <<= 1) {
        const u32 half = len / 2;
        const u32 stride = n / len;
        for (u32 i = 0; i < n; i += len) {
            for (u32 j = 0; j < half; ++j) {
                const std::complex<float> u = data[i + j];
                const std::complex<float> v = data[i + j + half] * twiddles[j * stride];
                data[i + j] = u + v;
                data[i + j + half] = u - v;
            }
        }
    }
}

void make_twiddles(u32 n, std::vector<std::complex<float>>& twiddles) {
    twiddles.resize(std::max<u32>(n / 2, 1));
    for (u32 k = 0; k < static_cast<u32>(twiddles.size()); ++k) {
        const double angle = -2.0 * 3.14159265358979323846 * static_cast<double>(k)
            / static_cast<double>(n);
        twiddles[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
}

} // namespace

void ConvReverbCpu::init(const float* ir_samples, u32 ir_length, u32 block_size) {
    m_irLength = 0;
    m_blockSize = 0;
    m_fftSize = 0;
    m_irSpectrum.clear();
    if (ir_samples == nullptr || ir_length == 0 || block_size == 0) {
        return;
    }

    m_irLength = ir_length;
    m_blockSize = block_size;
    m_fftSize = next_pow2(block_size + ir_length - 1);
    make_twiddles(m_fftSize, m_twiddles);

    m_irSpectrum.assign(m_fftSize, {0.f, 0.f});
    for (u32 i = 0; i < ir_length; ++i) {
        m_irSpectrum[i].real(ir_samples[i]);
    }
    fft_inplace(m_irSpectrum, m_twiddles);

    m_spectrum.assign(m_fftSize, {0.f, 0.f});
    m_overlap.assign(m_fftSize, 0.f);
    m_workInput.assign(m_fftSize, 0.f);
    m_workOutput.assign(m_fftSize, 0.f);
}

void ConvReverbCpu::reset() {
    std::fill(m_overlap.begin(), m_overlap.end(), 0.f);
}

void ConvReverbCpu::process(const float* input, float* output, u32 frames) {
    if (m_fftSize == 0) {
        std::fill(output, output + frames, 0.f);
        return;
    }
    // Blocks longer than the planned size would wrap the circular convolution — split them.
    while (frames > m_blockSize) {
        process_block_(input, output, m_blockSize);
        input += m_blockSize;
        output += m_blockSize;
        frames -= m_blockSize;
    }
    process_block_(input, output, frames);
}

void ConvReverbCpu::process_block_(const float* input, float* output, u32 frames) {
    for (u32 i = 0; i < m_fftSize; ++i) {
        m_spectrum[i] = {i < frames ? input[i] : 0.f, 0.f};
    }
    fft_inplace(m_spectrum, m_twiddles);
    for (u32 i = 0; i < m_fftSize; ++i) {
        // Inverse FFT via conj(FFT(conj(X))) / N.
        m_spectrum[i] = std::conj(m_spectrum[i] * m_irSpectrum[i]);
    }
    fft_inplace(m_spectrum, m_twiddles);
    const float inv_n = 1.f / static_cast<float>(m_fftSize);
    for (u32 i = 0; i < m_fftSize; ++i) {
        m_workOutput[i] = m_spectrum[i].real() * inv_n;
    }

    for (u32 i = 0; i < frames; ++i) {
        output[i] = m_workOutput[i] + m_overlap[i];
    }

    for (u32 i = 0; i < m_fftSize - frames; ++i) {
        m_overlap[i] = m_workOutput[i + frames] + m_overlap[i + frames];
    }
    for (u32 i = m_fftSize - frames; i < m_fftSize; ++i) {
        m_overlap[i] = 0.f;
    }
}

void ConvReverbCpu::convolve_direct(const float* input, u32 input_len, const float* ir, u32 ir_len,
                                    float* output) {
    const u32 out_len = input_len + ir_len - 1;
    for (u32 n = 0; n < out_len; ++n) {
        float sum = 0.f;
        const u32 k_min = n >= ir_len - 1 ? n - (ir_len - 1) : 0;
        const u32 k_max = n < input_len - 1 ? n : input_len - 1;
        for (u32 k = k_min; k <= k_max; ++k) {
            sum += input[k] * ir[n - k];
        }
        output[n] = sum;
    }
}

float ConvReverbCpu::peak_error_db(const float* a, const float* b, u32 length) {
    float peak = 0.f;
    for (u32 i = 0; i < length; ++i) {
        peak = std::max(peak, std::fabs(a[i] - b[i]));
    }
    if (peak <= 1e-12f) {
        return -120.f;
    }
    return 20.f * std::log10(peak);
}

} // namespace fuse::audio
