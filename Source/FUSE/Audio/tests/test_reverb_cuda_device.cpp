// B7 row "Convolution reverb CUDA FFT produces output within -60 dB noise floor of reference CPU FFT".
//
// For each case (1 s / 0.1 s / 2.5 s impulse responses at 48 kHz, steady and irregular block schedules) the
// reference is the CPU FFT reverb (ConvReverbCpu on CpuReference: the same partitioned FFT plan and
// single-source "reverb_fft" / "reverb_cmac" kernels on the CPU). CPU part (every build): the reference is
// within -120 dB of a double-precision direct convolution (shorter signal). Device part (RTX 3090):
// ReverbCuda runs the kernels on the CUDA device mirror; its output must be within -60 dB (peak error
// relative to the reference peak) of the CPU FFT reverb — the measured floor is printed — and every
// ReverbCuda::process() block is timed with CUDA events (report only; buffer period 10.67 ms @ 48 kHz for
// 512 frames). Exit 77 (after the CPU checks passed) without a CUDA device.

#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/audio/reverb_cuda.hpp>
#include <fuse/core/init.hpp>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

using fuse::u32;
using fuse::usize;
namespace audio = fuse::audio;
namespace kernel = fuse::kernel;

constexpr double kGateDb = -60.0;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::vector<float> makeIr(u32 length, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(-1.f, 1.f);
    std::vector<float> ir(length);
    for (u32 i = 0; i < length; ++i) {
        ir[i] = uni(rng) * std::exp(-static_cast<float>(i) / (0.25f * static_cast<float>(length)));
    }
    return ir;
}

std::vector<float> makeSignal(u32 length, u32 seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(-1.f, 1.f);
    std::vector<float> x(length);
    for (float& s : x) {
        s = uni(rng);
    }
    return x;
}

/// Peak |a - b| relative to the peak of `reference`, in dB.
double relativeErrorDb(const std::vector<float>& reference, const std::vector<float>& got) {
    double err = 0.0;
    double peak = 0.0;
    for (usize i = 0; i < reference.size(); ++i) {
        peak = std::max(peak, std::fabs(static_cast<double>(reference[i])));
        err = std::max(err, std::fabs(static_cast<double>(reference[i]) - static_cast<double>(got[i])));
    }
    return 20.0 * std::log10(std::max(err, 1e-30) / std::max(peak, 1e-30));
}

double directErrorDb(const std::vector<float>& got, const std::vector<float>& input, const std::vector<float>& ir,
                     usize frames) {
    double err = 0.0;
    double peak = 0.0;
    for (usize n = 0; n < frames; ++n) {
        double acc = 0.0;
        const usize kmax = std::min<usize>(n, ir.size() - 1u);
        for (usize k = 0; k <= kmax; ++k) {
            acc += static_cast<double>(input[n - k]) * ir[k];
        }
        peak = std::max(peak, std::fabs(acc));
        err = std::max(err, std::fabs(got[n] - acc));
    }
    return 20.0 * std::log10(std::max(err, 1e-30) / peak);
}

struct Case {
    const char* label;
    u32 irLength;
    u32 block;
    std::vector<u32> schedule;
    u32 frames;
};

template <typename Reverb>
std::vector<float> run(Reverb& reverb, const std::vector<float>& input, const std::vector<u32>& schedule,
                       std::vector<float>* blockMs) {
    std::vector<float> out(input.size(), 0.f);
    usize pos = 0;
#if defined(FUSE_HAS_CUDA)
    cudaEvent_t start = nullptr;
    cudaEvent_t stop = nullptr;
    const bool timed = blockMs != nullptr && cudaEventCreate(&start) == cudaSuccess && cudaEventCreate(&stop) == cudaSuccess;
#else
    (void)blockMs;
#endif
    for (usize k = 0; pos < input.size(); ++k) {
        const u32 frames = std::min<u32>(schedule[k % schedule.size()], static_cast<u32>(input.size() - pos));
#if defined(FUSE_HAS_CUDA)
        if (timed) {
            (void)cudaEventRecord(start, nullptr); // ReverbCuda's device mirror runs on the legacy default stream
        }
#endif
        reverb.process(input.data() + pos, out.data() + pos, frames);
#if defined(FUSE_HAS_CUDA)
        if (timed) {
            float ms = 0.f;
            if (cudaEventRecord(stop, nullptr) == cudaSuccess && cudaEventSynchronize(stop) == cudaSuccess &&
                cudaEventElapsedTime(&ms, start, stop) == cudaSuccess) {
                blockMs->push_back(ms);
            }
        }
#endif
        pos += frames;
    }
#if defined(FUSE_HAS_CUDA)
    if (start != nullptr) {
        (void)cudaEventDestroy(start);
    }
    if (stop != nullptr) {
        (void)cudaEventDestroy(stop);
    }
#endif
    return out;
}

} // namespace

int main() {
    fuse::core::initialize();
    const std::vector<Case> cases = {
        {"1 s IR, 512-frame blocks", 48000u, 512u, {512u}, 48000u * 3u},
        {"0.1 s IR, 256-frame blocks", 4800u, 256u, {256u}, 48000u},
        {"2.5 s IR, irregular blocks", 120000u, 480u, {480u, 17u, 480u, 301u, 1u, 480u}, 48000u * 4u},
    };
    bool deviceRan = false;
    for (const Case& c : cases) {
        const std::vector<float> ir = makeIr(c.irLength, c.irLength);
        const std::vector<float> input = makeSignal(c.frames, c.block);

        audio::ConvReverbCpu cpu;
        cpu.set_backend(kernel::Backend::CpuReference);
        cpu.init(ir.data(), c.irLength, c.block);
        const std::vector<float> reference = run(cpu, input, c.schedule, nullptr);
        const usize directFrames = std::min<usize>(input.size(), 6000u);
        const double cpuDirectDb = directErrorDb(reference, input, ir, directFrames);
        std::printf("%-28s CPU FFT reverb vs direct convolution (first %u frames): %.1f dB\n", c.label,
                    static_cast<unsigned>(directFrames), cpuDirectDb);
        expectTrue(cpuDirectDb <= -120.0, "CPU FFT reference within -120 dB of direct convolution");

        audio::ReverbCuda gpu;
        gpu.init(ir.data(), c.irLength, c.block);
        if (!gpu.available()) {
            continue;
        }
        deviceRan = true;
        std::vector<float> blockMs;
        const std::vector<float> out = run(gpu, input, c.schedule, &blockMs);
        const double db = relativeErrorDb(reference, out);
        std::sort(blockMs.begin(), blockMs.end());
        const float median = blockMs.empty() ? 0.f : blockMs[blockMs.size() / 2u];
        const float worst = blockMs.empty() ? 0.f : blockMs.back();
        std::printf("%-28s CUDA FFT reverb vs CPU FFT reverb: %.1f dB (gate %.0f dB); %llu blocks, median %.3f ms, "
                    "max %.3f ms per process() (CUDA events)\n",
                    c.label, db, kGateDb, static_cast<unsigned long long>(blockMs.size()), static_cast<double>(median), static_cast<double>(worst));
        expectTrue(db <= kGateDb, "CUDA FFT reverb within -60 dB of the CPU FFT reverb");
    }
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b7_reverb_cuda_device: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    if (!deviceRan) {
        std::printf("SKIP fuse_b7_reverb_cuda_device (device part): no CUDA device (CPU reference checks passed)\n");
        return 77;
    }
    std::printf("fuse_b7_reverb_cuda_device: passed\n");
    return EXIT_SUCCESS;
}
