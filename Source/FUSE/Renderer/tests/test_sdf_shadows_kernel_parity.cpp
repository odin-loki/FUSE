// Gate for the single-source `sdf_shadows` pass (fuse/renderer/shadow/sdf_shadow_kernel.hpp):
//   - CpuReference and CpuParallel (0/2/4 workers) produce bit-identical shadow factors on a 173x97 frame
//     (partial 8x8 tiles) of ground-plane surface points and background pixels, with and without normals.
//   - The kernel's occluder SDF is compute::ray_march_kernel::scene_eval: per-pixel results equal
//     sdfSoftShadow over that scene function evaluated here (same code, no copy).
//   - Launches record "sdf_shadows" stats; Cuda / Auto / VulkanCompute without a device fall back to
//     CpuParallel with the same image; invalid frames are rejected on every backend.

#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/shadow/sdf_shadow_kernel.hpp>
#include <fuse/renderer/shadow/sdf_shadows.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
namespace compute = fuse::compute;
namespace kernel = fuse::kernel;
using fuse::math::Vec3;
using fuse::math::Vec4;
using namespace fuse::renderer;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kWidth = 173;
constexpr u32 kHeight = 97;

compute::SdfObject makeObject(compute::SdfPrimitiveType type, Vec3 position, Vec3 params, f32 alpha,
                              f32 rounding = 0.f) {
    compute::SdfObject obj{};
    obj.type = static_cast<u32>(type);
    obj.position = position;
    obj.params = params;
    obj.alpha = alpha;
    obj.rounding = rounding;
    return obj;
}

const std::vector<compute::SdfObject>& occluders() {
    static const std::vector<compute::SdfObject> objects{
        makeObject(compute::SdfPrimitiveType::Sphere, {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.f),
        makeObject(compute::SdfPrimitiveType::Box, {2.2f, -0.3f, 0.4f}, {0.6f, 0.5f, 0.7f}, 0.35f, 0.15f),
        makeObject(compute::SdfPrimitiveType::Capsule, {-2.4f, 0.2f, 0.5f}, {0.35f, 0.8f, 0.f}, 0.25f),
        makeObject(compute::SdfPrimitiveType::Torus, {0.2f, 1.3f, 1.8f}, {0.8f, 0.2f, 0.f}, 0.f),
    };
    return objects;
}

struct Inputs {
    std::vector<Vec4> positions;
    std::vector<Vec3> normals;
};

/// Ground plane y = -1.2 under the occluders; every 11th pixel is background.
Inputs makeInputs() {
    Inputs in;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const f32 wx = -5.f + 10.f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kWidth);
            const f32 wz = -3.f + 7.f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kHeight);
            const bool background = (y * kWidth + x) % 11u == 0u;
            in.positions.push_back(Vec4{wx, -1.2f, wz, background ? 0.f : 1.f});
            in.normals.push_back({0.f, 1.f, 0.f});
        }
    }
    return in;
}

SdfShadowFrame makeFrame(const Inputs& in, std::vector<f32>& out, bool withNormals) {
    out.assign(kWidth * kHeight, -7.f);
    SdfShadowFrame frame{};
    frame.width = kWidth;
    frame.height = kHeight;
    frame.worldPositions = in.positions.data();
    frame.normals = withNormals ? in.normals.data() : nullptr;
    frame.objects = occluders().data();
    frame.objectCount = static_cast<u32>(occluders().size());
    frame.sceneMaxDistance = 50.f;
    frame.lightDirection = {0.35f, 1.f, 0.2f};
    frame.normalBias = 0.01f;
    frame.march.tMax = 30.f;
    frame.march.penumbraK = 8.f;
    frame.march.maxSteps = 96u;
    frame.outShadow = out.data();
    return frame;
}

std::vector<f32> render(kernel::Backend backend, const Inputs& in, bool withNormals) {
    std::vector<f32> out;
    expectTrue(renderSdfShadows(makeFrame(in, out, withNormals), backend), "sdf_shadows launch succeeds");
    return out;
}

void testBackendParity() {
    const Inputs in = makeInputs();
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    for (bool withNormals : {true, false}) {
        scheduler.shutdown();
        const std::vector<f32> reference = render(kernel::Backend::CpuReference, in, withNormals);
        u32 lit = 0u;
        u32 umbra = 0u;
        u32 penumbra = 0u;
        for (f32 s : reference) {
            (s >= 1.f ? lit : (s <= 0.f ? umbra : penumbra)) += 1u;
        }
        expectTrue(lit > 100u && umbra > 50u && penumbra > 100u, "frame has lit, umbra and penumbra pixels");
        for (u32 workers : {0u, 2u, 4u}) {
            scheduler.shutdown();
            scheduler.initialize(workers);
            const std::vector<f32> parallel = render(kernel::Backend::CpuParallel, in, withNormals);
            const kernel::ParityReport report =
                kernel::compare_bitwise(std::span<const f32>(reference), std::span<const f32>(parallel));
            char label[128];
            std::snprintf(label, sizeof(label), "sdf_shadows CpuReference == CpuParallel bit-exact (%u workers, %llu mismatches)",
                          workers, static_cast<unsigned long long>(report.mismatches));
            expectTrue(report.ok && report.compared == kWidth * kHeight, label);
        }

        // The kernel marches the ray march's scene SDF: same result as sdfSoftShadow over scene_eval here.
        std::vector<f32> scratch;
        const sdf_shadow_kernel::Params params = sdf_shadow_kernel::make_params(makeFrame(in, scratch, withNormals));
        const auto sceneSdf = [&params](const Vec3& p) {
            return compute::ray_march_kernel::scene_eval(params.scene, p, nullptr);
        };
        bool same = true;
        for (u32 i = 0; i < kWidth * kHeight; i += 7u) {
            const Vec4 s = in.positions[i];
            if (!(s.w > 0.f)) {
                same = same && reference[i] == 1.f;
                continue;
            }
            const Vec3 origin = withNormals ? Vec3{s.x, s.y, s.z} + in.normals[i] * params.normal_bias
                                            : Vec3{s.x, s.y, s.z};
            const f32 expected = sdfSoftShadow(sceneSdf, origin, params.light_dir, params.march);
            // Near rather than bitwise: this TU may contract (FMA) the SDF math differently than fuse_rhi.
            same = same && std::fabs(expected - reference[i]) <= 1e-5f;
        }
        expectTrue(same, "kernel pixels == sdfSoftShadow(ray_march_kernel::scene_eval)");
    }
    scheduler.shutdown();
}

void testStatsAndFallback() {
    const Inputs in = makeInputs();
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);
    kernel::reset_kernel_stats();
    const std::vector<f32> reference = render(kernel::Backend::CpuReference, in, true);
    kernel::KernelStats stats{};
    expectTrue(kernel::find_kernel_stats(sdf_shadow_kernel::kName, stats) && stats.launches == 1u &&
                   stats.items == kWidth * kHeight && stats.workgroups == 22u * 13u &&
                   stats.last_backend == kernel::Backend::CpuReference,
               "launch records sdf_shadows stats (items, 8x8 workgroups, backend)");

    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            const std::vector<f32> fallback = render(gpu, in, true);
            const kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel,
                       "GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(kernel::compare_bitwise(std::span<const f32>(reference), std::span<const f32>(fallback)).ok,
                       "fallback image == CpuReference image");
        }
    }

    std::vector<f32> out;
    SdfShadowFrame invalid = makeFrame(in, out, true);
    invalid.lightDirection = {};
    SdfShadowFrame noOutput = makeFrame(in, out, true);
    noOutput.outShadow = nullptr;
    for (kernel::Backend b : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel, kernel::Backend::Cuda,
                              kernel::Backend::Auto}) {
        expectTrue(!renderSdfShadows(invalid, b) && !renderSdfShadows(noOutput, b),
                   "invalid frames rejected on every backend");
    }
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParity();
    testStatsAndFallback();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d sdf_shadows kernel parity check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("SDF shadows kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
