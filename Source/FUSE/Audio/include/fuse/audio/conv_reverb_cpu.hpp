#pragma once

#include <fuse/audio/reverb_fft_kernel.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::audio {

struct ReverbFftDevice; // CUDA mirror of the delay line (kernels/reverb_fft.cu; CUDA builds only)

/// Uniformly partitioned FFT convolution reverb (overlap-save, frequency-domain delay line).
///
/// The impulse response is split into `partition_count()` partitions of `partition_size()` taps; every
/// block of up to `partition_size()` frames costs one "reverb_fft" transform of the new input window
/// (the older windows are reused from the delay line while blocks arrive at the partition hop), one
/// "reverb_cmac" spectral multiply-accumulate and one inverse "reverb_fft". Zero latency for any block
/// length; larger blocks are split. The single-source kernels (reverb_fft_kernel.hpp) run on
/// `backend()` (CpuReference by default: deterministic and job-free on the audio thread) or on a CUDA
/// device mirror (`enable_cuda`). Steady-state processing makes no heap allocations.
class ConvReverbCpu {
public:
    ConvReverbCpu() = default;
    ~ConvReverbCpu();
    ConvReverbCpu(const ConvReverbCpu&) = delete;
    ConvReverbCpu& operator=(const ConvReverbCpu&) = delete;
    ConvReverbCpu(ConvReverbCpu&& other) noexcept;
    ConvReverbCpu& operator=(ConvReverbCpu&& other) noexcept;

    void init(const float* ir_samples, u32 ir_length, u32 block_size);
    void reset();

    void process(const float* input, float* output, u32 frames);

    /// Backend of the kernel launches (CPU backends; a GPU request without a device mirror falls back
    /// to CpuParallel and is recorded as such in the kernel stats).
    void set_backend(kernel::Backend backend) { m_backend = backend; }
    [[nodiscard]] kernel::Backend backend() const { return m_backend; }

    /// Mirrors the plan on a CUDA device (CUDA build with a device only); false otherwise. `stream` is a
    /// cudaStream_t. Must be called after init(); init() / destroy drop the mirror.
    bool enable_cuda(void* stream = nullptr);
    [[nodiscard]] bool cuda_enabled() const { return m_device != nullptr; }

    u32 fft_size() const { return m_fftSize; }
    u32 ir_length() const { return m_irLength; }
    [[nodiscard]] u32 partition_size() const { return m_blockSize; }
    [[nodiscard]] u32 partition_count() const { return m_partitions; }

    /// Direct time-domain convolution for test reference.
    static void convolve_direct(const float* input, u32 input_len, const float* ir, u32 ir_len,
                                float* output);

    /// Peak error in dB between two buffers of equal length.
    static float peak_error_db(const float* a, const float* b, u32 length);

private:
    using Complex = reverb_fft::Complex;

    void process_block_(const float* input, float* output, u32 frames);
    /// Rotates the delay line for a block ending at `end` and lists its stale windows; returns the count.
    u32 schedule_windows_(u64 end);
    void release_device_();

    kernel::Backend m_backend = kernel::Backend::CpuReference;
    u32 m_fftSize = 0;    ///< N
    u32 m_log2Fft = 0;
    u32 m_irLength = 0;
    u32 m_blockSize = 0;  ///< partition size B (= largest block processed at once)
    u32 m_partitions = 0; ///< P
    u32 m_ringMask = 0;
    u32 m_head = 0;       ///< delay-line slot of partition 0
    u64 m_time = 0;       ///< input frames consumed since reset
    std::vector<Complex> m_twiddles;
    std::vector<Complex> m_irSpectra; ///< P x N
    std::vector<Complex> m_spectra;   ///< delay line: P x N input-window spectra
    std::vector<Complex> m_accum;     ///< N
    std::vector<float> m_ring;        ///< input history (power-of-two ring)
    std::vector<u64> m_slotEnd;       ///< window end (absolute frame) held by each delay-line slot
    std::vector<u32> m_staleStart;    ///< per-block scratch: windows to transform
    std::vector<u32> m_staleSlot;
    ReverbFftDevice* m_device = nullptr;
};

} // namespace fuse::audio
