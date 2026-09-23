// Gate for the single-source partitioned FFT reverb kernels (fuse/audio/reverb_fft_kernel.hpp):
//   - CpuReference and CpuParallel (0/2/4 workers) produce bit-identical reverb output for steady
//     blocks, irregular blocks (full delay-line rebuilds), oversize blocks (split) and a tiny plan whose
//     "reverb_cmac" grid is a partial workgroup; non-power-of-two block sizes and IR lengths.
//   - Output tracks a double-precision direct convolution (<= -120 dB of the peak) for every schedule.
//   - Steady-state blocks reuse the frequency-domain delay line (one forward + one inverse transform per
//     block) and make no heap allocations.
//   - Launches record "reverb_fft" / "reverb_cmac" stats; ReverbCuda without a device falls back to
//     CpuParallel (recorded) with output identical to the CPU reverb.
//   - Prints the per-block CPU timing (reference vs parallel).

#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/audio/reverb_cuda.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned long long> g_allocations{0};
} // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1u, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0u ? 1u : size)) {
        return p;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
    return operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::usize;
namespace audio = fuse::audio;
namespace kernel = fuse::kernel;

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
        ir[i] = uni(rng) * std::exp(-static_cast<float>(i) / (0.2f * static_cast<float>(length)));
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

/// Runs `input` through a reverb in chunks of `schedule` (cycled) frames.
std::vector<float> run(kernel::Backend backend, const std::vector<float>& ir, u32 block,
                       const std::vector<float>& input, const std::vector<u32>& schedule) {
    audio::ConvReverbCpu reverb;
    reverb.set_backend(backend);
    reverb.init(ir.data(), static_cast<u32>(ir.size()), block);
    std::vector<float> out(input.size(), -7.f);
    usize pos = 0;
    for (usize k = 0; pos < input.size(); ++k) {
        const u32 frames = std::min<u32>(schedule[k % schedule.size()], static_cast<u32>(input.size() - pos));
        reverb.process(input.data() + pos, out.data() + pos, frames);
        pos += frames;
    }
    return out;
}

double errorDb(const std::vector<float>& got, const std::vector<float>& input, const std::vector<float>& ir) {
    double err = 0.0;
    double peak = 0.0;
    for (usize n = 0; n < got.size(); ++n) {
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

bool bitEqual(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

struct Case {
    const char* label;
    u32 irLength;
    u32 block;
    std::vector<u32> schedule;
    u32 frames;
};

void testParityAndAccuracy() {
    const std::vector<Case> cases = {
        {"steady 512-frame blocks, ir 4800", 4800u, 512u, {512u}, 512u * 20u},
        {"irregular blocks (delay-line rebuilds)", 3001u, 480u, {480u, 17u, 480u, 480u, 301u, 1u, 480u}, 9000u},
        {"oversize blocks split", 2500u, 256u, {1000u, 256u, 700u}, 8000u},
        {"tiny plan (partial cmac workgroup)", 3u, 8u, {8u, 5u}, 200u},
        {"ir shorter than a block", 100u, 1024u, {1024u}, 5000u},
    };
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (const Case& c : cases) {
        const std::vector<float> ir = makeIr(c.irLength, c.irLength);
        const std::vector<float> input = makeSignal(c.frames, c.block);
        scheduler.shutdown();
        const std::vector<float> reference = run(kernel::Backend::CpuReference, ir, c.block, input, c.schedule);
        const double db = errorDb(reference, input, ir);
        std::printf("reverb_fft %-40s vs direct: %.1f dB\n", c.label, db);
        char label[160];
        std::snprintf(label, sizeof(label), "%s: within -120 dB of direct convolution", c.label);
        expectTrue(db <= -120.0, label);
        for (u32 workers : {0u, 2u, 4u}) {
            scheduler.shutdown();
            scheduler.initialize(workers);
            const std::vector<float> parallel = run(kernel::Backend::CpuParallel, ir, c.block, input, c.schedule);
            std::snprintf(label, sizeof(label), "%s: CpuReference == CpuParallel bit-exact (%u workers)", c.label,
                          workers);
            expectTrue(bitEqual(reference, parallel), label);
        }
    }
    scheduler.shutdown();
}

void testDelayLineReuseStatsAndAllocations() {
    const std::vector<float> ir = makeIr(4800u, 1u);
    const std::vector<float> input = makeSignal(512u * 16u, 2u);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.initialize(2);
    for (kernel::Backend backend : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel}) {
        audio::ConvReverbCpu reverb;
        reverb.set_backend(backend);
        reverb.init(ir.data(), 4800u, 512u);
        expectTrue(reverb.partition_size() == 512u && reverb.partition_count() == 10u && reverb.fft_size() == 1024u,
                   "plan: 10 partitions of 512 taps, 1024-point transforms");
        std::vector<float> out(input.size());
        for (u32 b = 0; b < 4u; ++b) { // warm-up: fills the delay line
            reverb.process(input.data() + b * 512u, out.data() + b * 512u, 512u);
        }
        kernel::reset_kernel_stats();
        const unsigned long long allocationsBefore = g_allocations.load();
        for (u32 b = 4u; b < 16u; ++b) {
            reverb.process(input.data() + b * 512u, out.data() + b * 512u, 512u);
        }
        const unsigned long long allocations = g_allocations.load() - allocationsBefore;
        kernel::KernelStats fft{};
        kernel::KernelStats cmac{};
        const bool found = kernel::find_kernel_stats(audio::reverb_fft::kFftName, fft) &&
                           kernel::find_kernel_stats(audio::reverb_fft::kCmacName, cmac);
        expectTrue(found && fft.launches == 24u && fft.workgroups == 24u && cmac.launches == 12u &&
                       cmac.items == 12u * 1024u && fft.last_backend == backend && cmac.last_backend == backend,
                   "steady state: one forward + one inverse reverb_fft workgroup and one reverb_cmac per block");
        std::printf("reverb steady state (%s): %llu heap allocations over 12 blocks\n", kernel::backend_name(backend),
                    allocations);
        expectTrue(allocations == 0u, "steady-state reverb blocks make no heap allocations");

        audio::ConvReverbCpu fresh;
        fresh.set_backend(backend);
        fresh.init(ir.data(), 4800u, 512u);
        std::vector<float> freshOut(input.size());
        fresh.process(input.data(), freshOut.data(), static_cast<u32>(input.size()));
        expectTrue(bitEqual(out, freshOut), "one large process() call == 512-frame blocks (same schedule)");
        reverb.reset();
        std::vector<float> again(input.size());
        reverb.process(input.data(), again.data(), static_cast<u32>(input.size()));
        expectTrue(bitEqual(out, again), "reset() restarts the stream exactly");
    }
    scheduler.shutdown();
}

void testReverbCudaFallback() {
    const std::vector<float> ir = makeIr(2000u, 3u);
    const std::vector<float> input = makeSignal(256u * 12u, 4u);
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.initialize(2);
    const std::vector<float> reference = run(kernel::Backend::CpuReference, ir, 256u, input, {256u});
    audio::ReverbCuda gpu;
    gpu.init(ir.data(), 2000u, 256u);
    std::vector<float> out(input.size());
    for (u32 b = 0; b < 12u; ++b) {
        gpu.process(input.data() + b * 256u, out.data() + b * 256u, 256u);
    }
    if (!gpu.available()) {
        const kernel::LaunchRecord last = kernel::last_launch();
        expectTrue(last.ok && last.requested == kernel::Backend::Cuda && last.backend == kernel::Backend::CpuParallel,
                   "ReverbCuda without a device: launches request Cuda and fall back to CpuParallel (recorded)");
        expectTrue(bitEqual(reference, out), "ReverbCuda fallback output == CPU reverb (bit-exact)");
    } else {
        std::printf("reverb_fft CUDA vs direct: %.1f dB\n", errorDb(out, input, ir));
        expectTrue(errorDb(out, input, ir) <= -110.0, "ReverbCuda (device) within -110 dB of direct convolution");
    }
    scheduler.shutdown();
}

void timeBlocks() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    const u32 workers = std::max(1u, std::thread::hardware_concurrency()) - 1u;
    scheduler.initialize(workers);
    for (u32 irLength : {4800u, 48000u}) {
        const std::vector<float> ir = makeIr(irLength, 9u);
        const std::vector<float> input = makeSignal(512u * 200u, 10u);
        std::vector<float> out(input.size());
        double ms[2] = {};
        const kernel::Backend backends[2] = {kernel::Backend::CpuReference, kernel::Backend::CpuParallel};
        for (int i = 0; i < 2; ++i) {
            audio::ConvReverbCpu reverb;
            reverb.set_backend(backends[i]);
            reverb.init(ir.data(), irLength, 512u);
            reverb.process(input.data(), out.data(), 512u * 8u); // warm-up
            const auto t0 = std::chrono::steady_clock::now();
            for (u32 b = 8u; b < 200u; ++b) {
                reverb.process(input.data() + b * 512u, out.data() + b * 512u, 512u);
            }
            ms[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 192.0;
        }
        std::printf("reverb block (512 frames, ir %u = %u partitions): CpuReference %.3f ms, CpuParallel (%u workers) "
                    "%.3f ms (buffer period 10.7 ms @ 48 kHz)\n",
                    irLength, (irLength + 511u) / 512u, ms[0], workers, ms[1]);
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testParityAndAccuracy();
    testDelayLineReuseStatsAndAllocations();
    testReverbCudaFallback();
    timeBlocks();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d reverb kernel check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("Reverb FFT kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
