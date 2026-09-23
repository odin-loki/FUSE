// Gate for the single-source skinning port (fuse/animation/skinning_kernel.hpp):
//   - CpuReference and CpuParallel (0/2/4 workers) skin a mesh with a partial edge workgroup
//     bit-identically (positions + normals; zero / negative weights and out-of-range bones included).
//   - Launches record "skinning_lbs" stats (items, 128-wide workgroups, backend).
//   - Cuda / Auto / VulkanCompute without a device fall back to CpuParallel (recorded) with the same
//     result; skinning_backend() / skinning_cuda_kernel_available() report that; invalid input is
//     rejected on every backend; steady-state calls reuse the output storage.

#include <fuse/animation/skinning.hpp>
#include <fuse/animation/skinning_kernel.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
namespace anim = fuse::animation;
namespace kernel = fuse::kernel;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// Deterministic LCG in [0, 1).
struct Rng {
    u32 state = 0x12345678u;
    f32 next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<f32>(state >> 8) / 16777216.f;
    }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * next(); }
};

anim::SkinningInput makeMesh(u32 vertexCount, u32 boneCount, bool normals) {
    Rng rng{};
    anim::SkinningInput input;
    for (u32 b = 0; b < boneCount; ++b) {
        anim::mat4 m = anim::mat4::identity();
        for (u32 i = 0; i < 12; ++i) {
            m.data[i] += rng.range(-0.4f, 0.4f);
        }
        m.data[12] = rng.range(-2.f, 2.f);
        m.data[13] = rng.range(-2.f, 2.f);
        m.data[14] = rng.range(-2.f, 2.f);
        input.bone_transforms.push_back(m);
    }
    for (u32 v = 0; v < vertexCount; ++v) {
        input.rest_positions.push_back({rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), 0.f});
        if (normals) {
            input.rest_normals.push_back({rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), 0.f});
        }
        anim::SkinningWeights w{};
        f32 sum = 0.f;
        for (u32 k = 0; k < 4; ++k) {
            w.bone_weights[k] = rng.next();
            sum += w.bone_weights[k];
            // Every 17th influence points past the palette (skipped); every 13th weight is negative.
            w.bone_indices[k] = ((v + k) % 17u == 0u) ? boneCount + 3u : static_cast<u32>(rng.next() * boneCount);
        }
        for (u32 k = 0; k < 4; ++k) {
            w.bone_weights[k] = ((v * 4u + k) % 13u == 0u) ? -w.bone_weights[k] : w.bone_weights[k] / sum;
        }
        if (v % 29u == 0u) {
            w = anim::SkinningWeights{}; // all weight on bone 0 (default)
        }
        input.weights.push_back(w);
    }
    return input;
}

kernel::ParityReport compareOutputs(const anim::SkinningOutput& a, const anim::SkinningOutput& b) {
    kernel::ParityReport report = kernel::compare_bitwise(std::span<const anim::vec3>(a.positions),
                                                          std::span<const anim::vec3>(b.positions));
    report.merge(kernel::compare_bitwise(std::span<const anim::vec3>(a.normals), std::span<const anim::vec3>(b.normals)));
    return report;
}

anim::SkinningOutput skin(kernel::Backend backend, const anim::SkinningInput& input) {
    anim::SkinningOutput out;
    expectTrue(anim::skin_vertices_on(backend, input, out), "skin_vertices_on succeeds");
    return out;
}

void testBackendParity() {
    constexpr u32 kVertices = 1000; // 7 full 128-wide workgroups + a partial edge workgroup
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (bool normals : {true, false}) {
        const anim::SkinningInput input = makeMesh(kVertices, 24, normals);
        scheduler.shutdown();
        const anim::SkinningOutput reference = skin(kernel::Backend::CpuReference, input);
        expectTrue(reference.positions.size() == kVertices && reference.normals.size() == (normals ? kVertices : 0u),
                   "reference output sized");
        for (u32 workers : {0u, 2u, 4u}) {
            scheduler.shutdown();
            scheduler.initialize(workers);
            const anim::SkinningOutput parallel = skin(kernel::Backend::CpuParallel, input);
            const kernel::ParityReport report = compareOutputs(reference, parallel);
            char label[160];
            std::snprintf(label, sizeof(label),
                          "CpuReference == CpuParallel bit-exact skinning (normals=%d, %u workers, %llu mismatches)",
                          normals ? 1 : 0, workers, static_cast<unsigned long long>(report.mismatches));
            expectTrue(report.ok && report.compared == kVertices * (normals ? 2u : 1u), label);
        }

        // The kernel body is the public API: a direct per-vertex call gives the same result.
        anim::SkinningOutput direct;
        direct.positions.resize(kVertices);
        direct.normals.resize(normals ? kVertices : 0u);
        const anim::skinning_kernel::Params params = anim::skinning_kernel::make_params(input, direct);
        for (u32 v = 0; v < kVertices; ++v) {
            anim::skinning_kernel::skin_vertex(params, v);
        }
        // Near rather than bitwise: this TU may contract (FMA) the inlined body differently.
        kernel::ParityReport near = kernel::compare_floats(std::span<const anim::vec3>(reference.positions),
                                                           std::span<const anim::vec3>(direct.positions), {1e-5, 1e-6});
        near.merge(kernel::compare_floats(std::span<const anim::vec3>(reference.normals),
                                          std::span<const anim::vec3>(direct.normals), {1e-5, 1e-6}));
        expectTrue(near.ok, "skin_vertex(v) == launched kernel output");
    }
    scheduler.shutdown();
}

void testStatsFallbackAndValidation() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    kernel::reset_kernel_stats();
    constexpr u32 kVertices = 300;
    const anim::SkinningInput input = makeMesh(kVertices, 8, true);
    const anim::SkinningOutput reference = skin(kernel::Backend::CpuReference, input);

    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(anim::skinning_kernel::kName, stats) && stats.launches == 1u &&
                   stats.items == kVertices && stats.workgroups == 3u &&
                   stats.last_backend == kernel::Backend::CpuReference,
               "launch records skinning_lbs stats (items, 128-wide workgroups, backend)");

    const bool device = kernel::backend_available(kernel::Backend::Cuda);
    expectTrue(anim::skinning_cuda_kernel_available() == device,
               "skinning_cuda_kernel_available() == compiled kernel && device present");
    expectTrue(anim::skinning_backend() == (device ? anim::SkinningBackend::Cuda : anim::SkinningBackend::CpuReference),
               "skinning_backend() reports the backend skin_vertices runs on");

    if (!device) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            const anim::SkinningOutput fallback = skin(gpu, input);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel,
                       "GPU skinning request without a device falls back to CpuParallel (recorded)");
            expectTrue(compareOutputs(reference, fallback).ok, "fallback skinning == CpuReference");
        }
        anim::SkinningOutput viaDefault;
        expectTrue(anim::skin_vertices(input, viaDefault), "skin_vertices succeeds");
        expectTrue(kernel::last_launch().requested == kernel::Backend::Auto, "skin_vertices launches Auto");
        expectTrue(compareOutputs(reference, viaDefault).ok, "skin_vertices == CpuReference");
    } else {
        const anim::SkinningOutput gpu = skin(kernel::Backend::Cuda, input);
        kernel::ParityReport report = kernel::compare_floats(std::span<const anim::vec3>(reference.positions),
                                                             std::span<const anim::vec3>(gpu.positions), {1e-4, 1e-5});
        report.merge(kernel::compare_floats(std::span<const anim::vec3>(reference.normals),
                                            std::span<const anim::vec3>(gpu.normals), {1e-4, 1e-5}));
        expectTrue(report.ok, "CUDA skinning within tolerance of CpuReference");
    }

    // Steady state: same-size calls reuse the output storage (no reallocation).
    anim::SkinningOutput out;
    expectTrue(anim::skin_vertices_on(kernel::Backend::CpuParallel, input, out), "first skin");
    const anim::vec3* positions = out.positions.data();
    const anim::vec3* normals = out.normals.data();
    for (int frame = 0; frame < 4; ++frame) {
        expectTrue(anim::skin_vertices_on(kernel::Backend::CpuParallel, input, out), "steady-state skin");
    }
    expectTrue(out.positions.data() == positions && out.normals.data() == normals,
               "steady-state skinning reuses output storage");

    anim::SkinningInput shortNormals = input;
    shortNormals.rest_normals.resize(5);
    anim::SkinningInput shortWeights = input;
    shortWeights.weights.resize(kVertices - 1u);
    const anim::SkinningInput empty{};
    const u64 launchesBefore = kernel::total_launch_count();
    for (kernel::Backend b : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel, kernel::Backend::Cuda,
                              kernel::Backend::Auto}) {
        anim::SkinningOutput o;
        expectTrue(!anim::skin_vertices_on(b, shortNormals, o), "short normal array rejected on every backend");
        expectTrue(!anim::skin_vertices_on(b, shortWeights, o), "short weight array rejected on every backend");
        expectTrue(!anim::skin_vertices_on(b, empty, o), "empty mesh rejected on every backend");
    }
    expectTrue(kernel::total_launch_count() == launchesBefore, "rejected input launches nothing");
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParity();
    testStatsFallbackAndValidation();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d skinning kernel parity check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("Skinning kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
