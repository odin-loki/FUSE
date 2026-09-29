// B7 row "GPU skinning transforms 10k vertices in < 0.5 ms — verified with CUDA events".
//
// CPU part (every build): the 10k-vertex / 64-bone gate mesh skins bit-identically on CpuReference and
// CpuParallel (the single-source kernel, fuse/animation/skinning_kernel.hpp); CPU time is reported.
// Device part (RTX 3090): time_skinning_cuda() stages the mesh once and times the same kernel body through
// the CUDA trampoline with CUDA events on resident buffers — single launches (median gated < 0.5 ms when
// timing budgets are enforced), a back-to-back batch, and palette-upload + launch "frames" — and the device
// result must match CpuReference within 1e-4 abs / 1e-5 rel. Exit 77 (after the CPU checks passed) when
// there is no CUDA kernel or device.

#include <fuse/animation/skinning.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <thread>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
namespace anim = fuse::animation;
namespace kernel = fuse::kernel;

constexpr u32 kVertices = 10000;
constexpr u32 kBones = 64;
constexpr f32 kBudgetMs = 0.5f;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// Deterministic LCG in [0, 1).
struct Rng {
    u32 state = 0x5eed1234u;
    f32 next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<f32>(state >> 8) / 16777216.f;
    }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * next(); }
};

/// Character-like mesh: 4 influences per vertex (normalised), 64-bone palette of rigid-ish transforms.
anim::SkinningInput makeMesh() {
    Rng rng{};
    anim::SkinningInput input;
    for (u32 b = 0; b < kBones; ++b) {
        anim::mat4 m = anim::mat4::identity();
        for (u32 i = 0; i < 12; ++i) {
            m.data[i] += rng.range(-0.2f, 0.2f);
        }
        m.data[12] = rng.range(-1.f, 1.f);
        m.data[13] = rng.range(-1.f, 1.f);
        m.data[14] = rng.range(-1.f, 1.f);
        input.bone_transforms.push_back(m);
    }
    for (u32 v = 0; v < kVertices; ++v) {
        input.rest_positions.push_back({rng.range(-1.f, 1.f), rng.range(0.f, 2.f), rng.range(-0.5f, 0.5f), 0.f});
        input.rest_normals.push_back({rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), 0.f});
        anim::SkinningWeights w{};
        f32 sum = 0.f;
        for (u32 k = 0; k < 4; ++k) {
            w.bone_indices[k] = static_cast<u32>(rng.next() * kBones) % kBones;
            w.bone_weights[k] = 0.05f + rng.next();
            sum += w.bone_weights[k];
        }
        for (f32& weight : w.bone_weights) {
            weight /= sum;
        }
        input.weights.push_back(w);
    }
    return input;
}

kernel::ParityReport compareOutputs(const anim::SkinningOutput& a, const anim::SkinningOutput& b, kernel::Tolerance tol,
                                    bool bitwise) {
    if (bitwise) {
        kernel::ParityReport report = kernel::compare_bitwise(std::span<const anim::vec3>(a.positions),
                                                              std::span<const anim::vec3>(b.positions));
        report.merge(kernel::compare_bitwise(std::span<const anim::vec3>(a.normals), std::span<const anim::vec3>(b.normals)));
        return report;
    }
    kernel::ParityReport report = kernel::compare_floats(std::span<const anim::vec3>(a.positions),
                                                         std::span<const anim::vec3>(b.positions), tol);
    report.merge(kernel::compare_floats(std::span<const anim::vec3>(a.normals), std::span<const anim::vec3>(b.normals), tol));
    return report;
}

double cpuMs(kernel::Backend backend, const anim::SkinningInput& input, anim::SkinningOutput& out) {
    (void)anim::skin_vertices_on(backend, input, out); // warm-up / sizing
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kRuns = 20;
    for (int i = 0; i < kRuns; ++i) {
        (void)anim::skin_vertices_on(backend, input, out);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRuns;
}

} // namespace

int main() {
    fuse::core::initialize();
    const anim::SkinningInput input = makeMesh();

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    const u32 workers = std::max(1u, std::thread::hardware_concurrency()) - 1u;
    scheduler.initialize(workers);
    anim::SkinningOutput reference;
    anim::SkinningOutput parallel;
    const double refMs = cpuMs(kernel::Backend::CpuReference, input, reference);
    const double parMs = cpuMs(kernel::Backend::CpuParallel, input, parallel);
    const kernel::ParityReport cpuParity = compareOutputs(reference, parallel, {}, true);
    std::printf("skinning %u vertices x %u bones: CpuReference %.3f ms, CpuParallel (%u workers) %.3f ms, "
                "%llu mismatches\n",
                kVertices, kBones, refMs, workers, parMs, static_cast<unsigned long long>(cpuParity.mismatches));
    expectTrue(reference.positions.size() == kVertices && reference.normals.size() == kVertices, "reference sized");
    expectTrue(cpuParity.ok, "CpuReference == CpuParallel bit-exact on the 10k-vertex gate mesh");

    anim::SkinningOutput device;
    const anim::SkinningCudaTiming timing = anim::time_skinning_cuda(input, device, 200u);
    int result = EXIT_SUCCESS;
    if (!timing.ran) {
        std::printf("SKIP fuse_b7_skinning_cuda_events (device part): %s\n", timing.reason);
        expectTrue(timing.reason[0] != '\0', "time_skinning_cuda explains why it did not run");
        result = g_failures == 0 ? 77 : EXIT_FAILURE;
    } else {
        const kernel::ParityReport gpuParity = compareOutputs(reference, device, {1e-4, 1e-5}, false);
        std::printf("CUDA skinning %u vertices: kernel min %.4f / median %.4f / max %.4f ms (CUDA events, %u launches), "
                    "back-to-back %.4f ms/launch, palette upload + kernel %.4f ms (median); one-time upload %.3f ms, "
                    "read-back %.3f ms; max abs error %.2e (%llu mismatches)\n",
                    timing.vertices, static_cast<double>(timing.kernel_ms_min),
                    static_cast<double>(timing.kernel_ms_median), static_cast<double>(timing.kernel_ms_max),
                    timing.iterations, static_cast<double>(timing.batch_ms_per_launch),
                    static_cast<double>(timing.frame_ms_median), static_cast<double>(timing.upload_ms),
                    static_cast<double>(timing.download_ms), gpuParity.max_abs_error,
                    static_cast<unsigned long long>(gpuParity.mismatches));
        expectTrue(gpuParity.ok, "CUDA skinning matches CpuReference (1e-4 abs / 1e-5 rel)");
        if (fuse::core::timingBudgetsEnforcedNoted()) {
            expectTrue(timing.kernel_ms_median < kBudgetMs, "GPU skinning of 10k vertices < 0.5 ms (median, CUDA events)");
        }
        result = g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    scheduler.shutdown();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b7_skinning_cuda_events: %d failure(s)\n", g_failures);
    } else if (result == EXIT_SUCCESS) {
        std::printf("fuse_b7_skinning_cuda_events: passed\n");
    }
    return result;
}
