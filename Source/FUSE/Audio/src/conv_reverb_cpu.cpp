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

void fft_inplace(std::vector<std::complex<float>>& data) {
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
        const float angle = -2.f * 3.14159265358979323846f / static_cast<float>(len);
        const std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (u32 i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (u32 j = 0; j < len / 2; ++j) {
                const std::complex<float> u = data[i + j];
                const std::complex<float> v = data[i + j + len / 2] * w;
                data[i + j] = u + v;
                data[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::vector<std::complex<float>> spectrum_from_real(const float* samples, u32 fft_size) {
    std::vector<std::complex<float>> data(fft_size, {0.f, 0.f});
    for (u32 i = 0; i < fft_size; ++i) {
        data[i].real(samples[i]);
    }
    fft_inplace(data);
    return data;
}

void multiply_spectrum(std::vector<std::complex<float>>& a, const std::vector<std::complex<float>>& b) {
    for (usize i = 0; i < a.size(); ++i) {
        a[i] *= b[i];
    }
}

void ifft_to_real(const std::vector<std::complex<float>>& freq, float* out, u32 fft_size) {
    std::vector<std::complex<float>> data = freq;
    const float inv_n = 1.f / static_cast<float>(fft_size);
    for (auto& value : data) {
        value = std::conj(value);
    }
    fft_inplace(data);
    for (u32 i = 0; i < fft_size; ++i) {
        out[i] = data[i].real() * inv_n;
    }
}

} // namespace

void ConvReverbCpu::init(const float* ir_samples, u32 ir_length, u32 block_size) {
    m_irLength = ir_length;
    m_blockSize = block_size;
    m_fftSize = next_pow2(block_size + ir_length - 1);

    std::vector<float> padded(m_fftSize, 0.f);
    std::memcpy(padded.data(), ir_samples, ir_length * sizeof(float));
    m_irSpectrum = spectrum_from_real(padded.data(), m_fftSize);

    m_overlap.assign(m_fftSize, 0.f);
    m_workInput.assign(m_fftSize, 0.f);
    m_workOutput.assign(m_fftSize, 0.f);
}

void ConvReverbCpu::reset() {
    std::fill(m_overlap.begin(), m_overlap.end(), 0.f);
}

void ConvReverbCpu::process(const float* input, float* output, u32 frames) {
    std::fill(m_workInput.begin(), m_workInput.end(), 0.f);
    std::memcpy(m_workInput.data(), input, frames * sizeof(float));

    auto input_spectrum = spectrum_from_real(m_workInput.data(), m_fftSize);
    multiply_spectrum(input_spectrum, m_irSpectrum);
    ifft_to_real(input_spectrum, m_workOutput.data(), m_fftSize);

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
