// Partitioned FFT convolution reverb. The transforms and the spectral multiply-accumulate are the
// single-source kernels in fuse/audio/reverb_fft_kernel.hpp ("reverb_fft", "reverb_cmac"); this TU only
// plans the partitions, keeps the input ring / delay-line bookkeeping and launches them.

#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace fuse::audio {

#if defined(FUSE_HAS_CUDA)
/// kernels/reverb_fft.cu: device mirror of the plan + delay line running the same kernel bodies.
ReverbFftDevice* reverb_fft_device_create(const reverb_fft::Plan& plan, void* stream);
void reverb_fft_device_destroy(ReverbFftDevice* device);
bool reverb_fft_device_block(ReverbFftDevice* device, const reverb_fft::Block& block);
void reverb_fft_device_reset(ReverbFftDevice* device);
#endif

namespace {

constexpr u64 kNoWindow = ~0ull;

u32 next_pow2(u32 value) {
    u32 result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

u32 log2_pow2(u32 value) {
    u32 bits = 0;
    while ((1u << bits) < value) {
        ++bits;
    }
    return bits;
}

} // namespace

ConvReverbCpu::~ConvReverbCpu() {
    release_device_();
}

ConvReverbCpu::ConvReverbCpu(ConvReverbCpu&& other) noexcept {
    *this = std::move(other);
}

ConvReverbCpu& ConvReverbCpu::operator=(ConvReverbCpu&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release_device_();
    m_backend = other.m_backend;
    m_fftSize = other.m_fftSize;
    m_log2Fft = other.m_log2Fft;
    m_irLength = other.m_irLength;
    m_blockSize = other.m_blockSize;
    m_partitions = other.m_partitions;
    m_ringMask = other.m_ringMask;
    m_head = other.m_head;
    m_time = other.m_time;
    m_twiddles = std::move(other.m_twiddles);
    m_irSpectra = std::move(other.m_irSpectra);
    m_spectra = std::move(other.m_spectra);
    m_accum = std::move(other.m_accum);
    m_ring = std::move(other.m_ring);
    m_slotEnd = std::move(other.m_slotEnd);
    m_staleStart = std::move(other.m_staleStart);
    m_staleSlot = std::move(other.m_staleSlot);
    m_device = std::exchange(other.m_device, nullptr);
    other.m_fftSize = 0;
    other.m_irLength = 0;
    other.m_blockSize = 0;
    other.m_partitions = 0;
    return *this;
}

void ConvReverbCpu::release_device_() {
#if defined(FUSE_HAS_CUDA)
    if (m_device != nullptr) {
        reverb_fft_device_destroy(m_device);
    }
#endif
    m_device = nullptr;
}

void ConvReverbCpu::init(const float* ir_samples, u32 ir_length, u32 block_size) {
    release_device_();
    m_irLength = 0;
    m_blockSize = 0;
    m_fftSize = 0;
    m_partitions = 0;
    m_irSpectra.clear();
    if (ir_samples == nullptr || ir_length == 0 || block_size == 0) {
        return;
    }

    // Partition = hop = largest block processed at once; overlap-save needs N >= 2B - 1.
    m_irLength = ir_length;
    m_blockSize = std::min(block_size, reverb_fft::kMaxPartition);
    m_fftSize = std::max(next_pow2(2u * m_blockSize - 1u), 2u);
    m_log2Fft = log2_pow2(m_fftSize);
    m_partitions = (ir_length + m_blockSize - 1u) / m_blockSize;
    const u32 ringSize = next_pow2((m_partitions - 1u) * m_blockSize + m_fftSize);
    m_ringMask = ringSize - 1u;

    // w_k = exp(-2 pi i k / N) in double precision so the table error does not grow with N.
    m_twiddles.resize(std::max<u32>(m_fftSize / 2u, 1u));
    for (u32 k = 0; k < static_cast<u32>(m_twiddles.size()); ++k) {
        const double angle = -2.0 * 3.14159265358979323846 * static_cast<double>(k) / static_cast<double>(m_fftSize);
        m_twiddles[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }

    const usize bins = static_cast<usize>(m_partitions) * m_fftSize;
    m_irSpectra.assign(bins, Complex{});
    m_spectra.assign(bins, Complex{});
    m_accum.assign(m_fftSize, Complex{});
    m_ring.assign(ringSize, 0.f);
    m_slotEnd.assign(m_partitions, kNoWindow);
    m_staleStart.assign(m_partitions, 0u);
    m_staleSlot.assign(m_partitions, 0u);
    m_head = 0;
    m_time = 0;

    // IR partition spectra: one "reverb_fft" workgroup per partition (segment mode).
    reverb_fft::FftParams params{};
    params.twiddles = kernel::make_span<const Complex>(m_twiddles.data(), static_cast<u32>(m_twiddles.size()));
    params.n = m_fftSize;
    params.log2n = m_log2Fft;
    params.mode = reverb_fft::FftMode::Segment;
    params.src = kernel::make_span(ir_samples, ir_length);
    params.partition = m_blockSize;
    params.spectra = kernel::make_span(m_irSpectra.data(), static_cast<u32>(bins));
    kernel::launch(m_backend, reverb_fft::make_fft_launch(m_fftSize, m_partitions), reverb_fft::FftKernel{}, params);
}

bool ConvReverbCpu::enable_cuda(void* stream) {
#if defined(FUSE_HAS_CUDA)
    if (m_fftSize == 0 || !kernel::backend_available(kernel::Backend::Cuda)) {
        return false;
    }
    release_device_();
    reverb_fft::Plan plan{};
    plan.twiddles = kernel::make_span<const Complex>(m_twiddles.data(), static_cast<u32>(m_twiddles.size()));
    plan.ir_spectra = kernel::make_span<const Complex>(m_irSpectra.data(), static_cast<u32>(m_irSpectra.size()));
    plan.n = m_fftSize;
    plan.log2n = m_log2Fft;
    plan.partition = m_blockSize;
    plan.partitions = m_partitions;
    plan.ring_size = m_ringMask + 1u;
    m_device = reverb_fft_device_create(plan, stream);
    reset();
    return m_device != nullptr;
#else
    (void)stream;
    return false;
#endif
}

void ConvReverbCpu::reset() {
    std::fill(m_ring.begin(), m_ring.end(), 0.f);
    std::fill(m_slotEnd.begin(), m_slotEnd.end(), kNoWindow);
    m_head = 0;
    m_time = 0;
#if defined(FUSE_HAS_CUDA)
    if (m_device != nullptr) {
        reverb_fft_device_reset(m_device);
    }
#endif
}

void ConvReverbCpu::process(const float* input, float* output, u32 frames) {
    if (m_fftSize == 0) {
        std::fill(output, output + frames, 0.f);
        return;
    }
    // Blocks longer than a partition are split (the overlap-save window holds at most B new frames).
    while (frames > m_blockSize) {
        process_block_(input, output, m_blockSize);
        input += m_blockSize;
        output += m_blockSize;
        frames -= m_blockSize;
    }
    if (frames > 0u) {
        process_block_(input, output, frames);
    }
}

u32 ConvReverbCpu::schedule_windows_(u64 end) {
    // Frequency-domain delay line: when the previous block ended exactly one hop ago, partition p's
    // window is the previous block's partition p - 1 window, so rotating the head leaves one stale
    // window (the newest). Slots are tagged with their window end, so any other block length simply
    // re-transforms whatever no longer matches.
    const u32 partitions = m_partitions;
    if (m_slotEnd[m_head] + m_blockSize == end) {
        m_head = m_head == 0u ? partitions - 1u : m_head - 1u;
    }
    u32 stale = 0;
    u32 slot = m_head;
    for (u32 p = 0; p < partitions; ++p) {
        const u64 windowEnd = end - static_cast<u64>(p) * m_blockSize; // modular: early windows are zeros
        if (m_slotEnd[slot] != windowEnd) {
            m_slotEnd[slot] = windowEnd;
            m_staleStart[stale] = static_cast<u32>(windowEnd - m_fftSize);
            m_staleSlot[stale] = slot;
            ++stale;
        }
        slot = slot + 1u == partitions ? 0u : slot + 1u;
    }
    return stale;
}

void ConvReverbCpu::process_block_(const float* input, float* output, u32 frames) {
    const u32 n = m_fftSize;
    const u64 end = m_time + frames; // window end of partition 0
    for (u32 i = 0; i < frames; ++i) {
        m_ring[static_cast<u32>(m_time + i) & m_ringMask] = input[i];
    }
    u32 stale = schedule_windows_(end);

    reverb_fft::Block block{};
    block.input = input;
    block.output = output;
    block.frames = frames;
    block.time = m_time;
    block.stale_start = kernel::make_span<const u32>(m_staleStart.data(), stale);
    block.stale_slot = kernel::make_span<const u32>(m_staleSlot.data(), stale);
    block.head = m_head;
    m_time = end;

#if defined(FUSE_HAS_CUDA)
    if (m_device != nullptr) {
        if (reverb_fft_device_block(m_device, block)) {
            return;
        }
        // Device failure: continue on the host; its delay line was never filled, so rebuild all of it.
        release_device_();
        std::fill(m_slotEnd.begin(), m_slotEnd.end(), kNoWindow);
        stale = schedule_windows_(end);
        block.stale_start.size = stale;
        block.stale_slot.size = stale;
        block.head = m_head;
    }
#endif

    reverb_fft::Plan plan{};
    plan.twiddles = kernel::make_span<const Complex>(m_twiddles.data(), static_cast<u32>(m_twiddles.size()));
    plan.ir_spectra = kernel::make_span<const Complex>(m_irSpectra.data(), static_cast<u32>(m_irSpectra.size()));
    plan.n = n;
    plan.log2n = m_log2Fft;
    plan.partition = m_blockSize;
    plan.partitions = m_partitions;
    plan.ring_size = m_ringMask + 1u;
    const kernel::Span<Complex> spectra = kernel::make_span(m_spectra.data(), static_cast<u32>(m_spectra.size()));
    const kernel::Span<Complex> accum = kernel::make_span(m_accum.data(), n);

    if (stale > 0u) {
        kernel::launch(m_backend, reverb_fft::make_fft_launch(n, stale), reverb_fft::FftKernel{},
                       reverb_fft::make_window_params(
                           plan, kernel::make_span<const f32>(m_ring.data(), static_cast<u32>(m_ring.size())),
                           block.stale_start, block.stale_slot, spectra));
    }
    kernel::launch(m_backend, reverb_fft::make_cmac_launch(n), reverb_fft::CmacKernel{},
                   reverb_fft::make_cmac_params(plan, block, kernel::make_span<const Complex>(spectra.data, spectra.size),
                                                accum));
    kernel::launch(m_backend, reverb_fft::make_fft_launch(n, 1u), reverb_fft::FftKernel{},
                   reverb_fft::make_inverse_params(plan, block, kernel::make_span<const Complex>(accum.data, n),
                                                   kernel::make_span(output, frames)));
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
