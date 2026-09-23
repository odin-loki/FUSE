#pragma once

// Single-source uniformly partitioned FFT convolution (docs/compute-kernels.md). This header is
// device-safe and holds the ONLY implementation of the reverb's FFT and spectral multiply-accumulate:
// ConvReverbCpu (conv_reverb_cpu.cpp, CPU backends), ReverbCuda's CPU fallback and the CUDA path
// (kernels/reverb_fft.cu) all launch these bodies.
//
// The impulse response is split into P partitions of B taps. Each block of f <= B input frames is
// convolved by overlap-save with an N = pow2 >= 2B - 1 point FFT:
//   "reverb_fft"  workgroup kernel, one workgroup per transform. Phase 0 loads the (bit-reversed)
//                 window into scratch, phases 1..log2(N) run one radix-2 butterfly stage each, the last
//                 phase stores. It computes the input-window spectra of the frequency-domain delay line
//                 (only the stale ones: one per block in steady state), the IR partition spectra at init,
//                 and the inverse transform of the accumulated spectrum (conj trick) writing the block's
//                 f output samples.
//   "reverb_cmac" item kernel, one bin per item: acc[k] = sum_p X_p[k] * H_p[k] in fixed p order.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

namespace fuse::audio::reverb_fft {

inline constexpr const char* kFftName = "reverb_fft";
inline constexpr const char* kCmacName = "reverb_cmac";

/// Largest transform: N = 2048 complex f32 = 16 KiB, the portable workgroup scratch limit.
inline constexpr u32 kMaxLog2N = 11;
inline constexpr u32 kMaxFftSize = 1u << kMaxLog2N;
/// Largest partition (hop) so that 2B - 1 <= kMaxFftSize.
inline constexpr u32 kMaxPartition = kMaxFftSize / 2u;
/// FFT workgroup: threads stride over the N/2 butterflies of a stage (<= 256 for Vulkan portability).
inline constexpr u32 kFftThreads = 256;
inline constexpr kernel::Dim3 kCmacWorkgroup{64u, 1u, 1u};

struct Complex {
    f32 re = 0.f;
    f32 im = 0.f;
};

FUSE_HOST_DEVICE inline Complex cmul(Complex a, Complex b) {
    return Complex{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

FUSE_HOST_DEVICE inline u32 bit_reverse(u32 value, u32 bits) {
    u32 result = 0;
    for (u32 i = 0; i < bits; ++i) {
        result = (result << 1u) | ((value >> i) & 1u);
    }
    return result;
}

enum class FftMode : u32 {
    RingWindow = 0, ///< forward: x[i] = ring[(window_start[g] + i) & ring_mask] -> spectra[slot[g]]
    Segment = 1,    ///< forward: x[i] = src[g * B + i] for i < min(B, src_len - g * B), else 0 -> spectra[g]
    Inverse = 2,    ///< inverse of `accum`: out[j] = Re(ifft)[N - out_count + j] (overlap-save tail)
};

struct FftParams {
    kernel::Span<const Complex> twiddles{}; ///< w_k = exp(-2 pi i k / N), k < N/2
    u32 n = 0;
    u32 log2n = 0;
    FftMode mode = FftMode::RingWindow;
    // RingWindow
    kernel::Span<const f32> ring{};
    u32 ring_mask = 0;
    kernel::Span<const u32> window_start{}; ///< per workgroup (absolute sample index; masked on read)
    kernel::Span<const u32> slot{};         ///< per workgroup destination slot in `spectra`
    // Segment
    kernel::Span<const f32> src{};
    u32 partition = 0; ///< B
    // RingWindow / Segment destination: slot-major, N bins per slot.
    kernel::Span<Complex> spectra{};
    // Inverse
    kernel::Span<const Complex> accum{};
    kernel::Span<f32> out{};
    u32 out_count = 0;
    f32 scale = 1.f; ///< 1/N
};

/// Radix-2 DIT FFT of one transform per workgroup, one butterfly stage per phase.
struct FftKernel {
    using Scratch = Complex;
    static constexpr u32 kScratchCount = kMaxFftSize;
    static constexpr u32 kPhases = kMaxLog2N + 2u; // load, log2(N) stages (unused ones idle), store

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const kernel::WorkgroupContext<Complex>& wg,
                                     const FftParams& p) const {
        const u32 g = idx.group.x;
        const u32 tid = idx.local_linear;
        const u32 threads = idx.workgroup.x;
        Complex* s = wg.scratch;
        if (wg.phase == 0u) {
            for (u32 i = tid; i < p.n; i += threads) {
                Complex x{};
                if (p.mode == FftMode::RingWindow) {
                    x.re = p.ring[(p.window_start[g] + i) & p.ring_mask];
                } else if (p.mode == FftMode::Segment) {
                    const u32 base = g * p.partition;
                    if (i < p.partition && base + i < p.src.size) {
                        x.re = p.src[base + i];
                    }
                } else {
                    x = p.accum[i];
                    x.im = -x.im; // inverse via conj(FFT(conj(X))) / N
                }
                s[bit_reverse(i, p.log2n)] = x;
            }
            return;
        }
        if (wg.phase <= p.log2n) {
            const u32 half = 1u << (wg.phase - 1u);
            const u32 stride = p.n >> wg.phase;
            for (u32 b = tid; b < p.n / 2u; b += threads) {
                const u32 j = b & (half - 1u);
                const u32 i0 = ((b >> (wg.phase - 1u)) << wg.phase) + j;
                const u32 i1 = i0 + half;
                const Complex u = s[i0];
                const Complex v = cmul(s[i1], p.twiddles[j * stride]);
                s[i0] = Complex{u.re + v.re, u.im + v.im};
                s[i1] = Complex{u.re - v.re, u.im - v.im};
            }
            return;
        }
        if (wg.phase != kPhases - 1u) {
            return;
        }
        if (p.mode == FftMode::Inverse) {
            const u32 first = p.n - p.out_count;
            for (u32 j = tid; j < p.out_count; j += threads) {
                p.out[j] = s[first + j].re * p.scale; // Re(conj(z)) == Re(z)
            }
            return;
        }
        const u32 slot = p.mode == FftMode::RingWindow ? p.slot[g] : g;
        for (u32 i = tid; i < p.n; i += threads) {
            p.spectra[slot * p.n + i] = s[i];
        }
    }
};

struct CmacParams {
    kernel::Span<const Complex> spectra{};    ///< P input-window spectra (delay line), slot-major
    kernel::Span<const Complex> ir_spectra{}; ///< P IR partition spectra, partition-major
    kernel::Span<Complex> accum{};            ///< N bins
    u32 n = 0;
    u32 partitions = 0;
    u32 head = 0; ///< slot of partition 0 (the newest window); partition p lives in (head + p) % P
};

/// Complex multiply-accumulate over the delay line, one bin per item (fixed partition order).
struct CmacKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const CmacParams& p) const {
        const u32 k = idx.linear;
        Complex acc{};
        u32 slot = p.head;
        for (u32 part = 0; part < p.partitions; ++part) {
            const Complex prod = cmul(p.spectra[slot * p.n + k], p.ir_spectra[part * p.n + k]);
            acc.re += prod.re;
            acc.im += prod.im;
            slot = slot + 1u == p.partitions ? 0u : slot + 1u;
        }
        p.accum[k] = acc;
    }
};

/// One workgroup per transform; threads = min(N/2, kFftThreads).
inline kernel::KernelLaunch make_fft_launch(u32 n, u32 transforms) {
    const u32 threads = n / 2u < kFftThreads ? (n / 2u > 0u ? n / 2u : 1u) : kFftThreads;
    return kernel::KernelLaunch{kFftName, kernel::extent1(transforms * threads), kernel::Dim3{threads, 1u, 1u}};
}

inline kernel::KernelLaunch make_cmac_launch(u32 n) {
    return kernel::KernelLaunch{kCmacName, kernel::extent1(n), kCmacWorkgroup};
}

/// Host-side description of a planned convolution (for device mirrors: kernels/reverb_fft.cu).
struct Plan {
    kernel::Span<const Complex> twiddles{};
    kernel::Span<const Complex> ir_spectra{}; ///< partitions x n
    u32 n = 0;
    u32 log2n = 0;
    u32 partition = 0;
    u32 partitions = 0;
    u32 ring_size = 0;
};

/// One block of the overlap-save schedule, resolved on the host (the same for every executor).
struct Block {
    const f32* input = nullptr;
    f32* output = nullptr;
    u32 frames = 0;
    u64 time = 0; ///< absolute frame index of input[0]
    kernel::Span<const u32> stale_start{};
    kernel::Span<const u32> stale_slot{};
    u32 head = 0;
};

inline FftParams make_window_params(const Plan& plan, kernel::Span<const f32> ring, kernel::Span<const u32> start, kernel::Span<const u32> slot,
                                    kernel::Span<Complex> spectra) {
    FftParams p{};
    p.twiddles = plan.twiddles;
    p.n = plan.n;
    p.log2n = plan.log2n;
    p.mode = FftMode::RingWindow;
    p.ring = ring;
    p.ring_mask = plan.ring_size - 1u;
    p.window_start = start;
    p.slot = slot;
    p.spectra = spectra;
    return p;
}

inline CmacParams make_cmac_params(const Plan& plan, const Block& block, kernel::Span<const Complex> spectra,
                                   kernel::Span<Complex> accum) {
    CmacParams p{};
    p.spectra = spectra;
    p.ir_spectra = plan.ir_spectra;
    p.accum = accum;
    p.n = plan.n;
    p.partitions = plan.partitions;
    p.head = block.head;
    return p;
}

inline FftParams make_inverse_params(const Plan& plan, const Block& block, kernel::Span<const Complex> accum,
                                     kernel::Span<f32> out) {
    FftParams p{};
    p.twiddles = plan.twiddles;
    p.n = plan.n;
    p.log2n = plan.log2n;
    p.mode = FftMode::Inverse;
    p.accum = accum;
    p.out = out;
    p.out_count = block.frames;
    p.scale = 1.f / static_cast<f32>(plan.n);
    return p;
}

} // namespace fuse::audio::reverb_fft
