// Gate for the single-source SSAO / SSR / SSGI port (fuse/ssfx/{hbao,ssr,ssgi}_kernel.hpp and
// fuse/compute/screen_space_kernels.hpp):
//   - CpuReference and CpuParallel (0/2/4 workers) produce bit-identical AO (raw + blurred, buffer and
//     reconstructed normals), SSR (mirror + roughness / contact hardening) and SSGI (1-3 bounces) surfaces on a
//     frame with partial 8x8 edge tiles, sky pixels and depth discontinuities — through both fuse_compute's
//     launch_*_cpu_backend and fuse_ssfx's full-frame compute*Cpu.
//   - The kernel pixels equal the public scalar API (hbaoPixelVisibility / ssrTracePixel) — one implementation.
//   - Launches record "screen_space_ao", "screen_space_ao_blur", "screen_space_reflections" and
//     "screen_space_gi" (one per bounce) stats; Cuda / Auto / VulkanCompute without a device fall back to
//     CpuParallel with the same surfaces; invalid params are rejected on every backend.

#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute/screen_space_kernels.hpp>
#include <fuse/compute_kernel/parity.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/ssfx/hbao.hpp>
#include <fuse/ssfx/ssgi.hpp>
#include <fuse/ssfx/ssr.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
namespace compute = fuse::compute;
namespace kernel = fuse::kernel;
namespace ssfx = fuse::ssfx;
using fuse::math::Vec3;
using fuse::math::Vec4;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kW = 77; // not multiples of the 8x8 workgroup: partial edge tiles
constexpr u32 kH = 53;
constexpr u32 kPixels = kW * kH;

/// Floor + a fronto-parallel wall (depth discontinuity), sky above the horizon, a tilted-normal patch.
/// Normals are engine view space (+Y up, -Z forward); colour / albedo / roughness vary per pixel.
struct Scene {
    f32 proj[16]{};
    std::vector<f32> depth = std::vector<f32>(kPixels);
    std::vector<f32> roughness = std::vector<f32>(kPixels);
    std::vector<Vec3> normals = std::vector<Vec3>(kPixels);
    std::vector<Vec3> color = std::vector<Vec3>(kPixels);
    std::vector<Vec3> albedo = std::vector<Vec3>(kPixels);

    Scene() {
        const f32 fov = 1.f;
        const f32 nearZ = 0.1f;
        const f32 farZ = 100.f;
        const f32 f = 1.f / std::tan(0.5f * fov);
        proj[0] = f * static_cast<f32>(kH) / static_cast<f32>(kW);
        proj[5] = f;
        proj[10] = farZ / (nearZ - farZ);
        proj[11] = -1.f;
        proj[14] = nearZ * farZ / (nearZ - farZ);
        const f32 fx = 0.5f * static_cast<f32>(kW) * proj[0];
        const f32 fy = 0.5f * static_cast<f32>(kH) * proj[5];
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                const u32 i = y * kW + x;
                const f32 dx = (static_cast<f32>(x) + 0.5f - 0.5f * kW) / fx;
                const f32 dy = (static_cast<f32>(y) + 0.5f - 0.5f * kH) / fy; // +Y down
                f32 z = 0.f;
                Vec3 n{0.f, 1.f, 0.f};
                Vec3 c{};
                const f32 wallZ = 6.f;
                if (dx * wallZ > -1.2f && dx * wallZ < 0.6f && dy * wallZ > -1.5f && dy * wallZ < 1.f) {
                    z = wallZ;
                    n = {0.f, 0.f, 1.f};
                    c = {0.1f, 0.9f, 0.2f};
                } else if (dy > 0.f) {
                    z = 1.f / dy;
                    z = z > 40.f ? 0.f : z;
                    c = {0.5f + 0.4f * std::sin(dx * z), 0.4f, 0.3f + 0.2f * std::cos(z)};
                }
                if (x > 50 && y > 35 && z > 0.f) {
                    n = Vec3{0.3f, 0.9f, 0.1f}.normalized();
                }
                depth[i] = z;
                normals[i] = n;
                color[i] = c;
                roughness[i] = static_cast<f32>(x % 7u) * 0.17f;
                albedo[i] = {0.8f, 0.6f + 0.01f * static_cast<f32>(y % 5u), 0.5f};
            }
        }
    }
};

const Scene& scene() {
    static const Scene s;
    return s;
}

compute::SSAOParams ssaoParams(std::vector<f32>& out, bool blur, bool normals) {
    const Scene& s = scene();
    compute::SSAOParams p{};
    p.depth_surface = const_cast<f32*>(s.depth.data());
    p.normal_surface = normals ? const_cast<Vec3*>(s.normals.data()) : nullptr;
    p.ao_out_surface = out.data();
    p.width = kW;
    p.height = kH;
    std::memcpy(p.proj, s.proj, sizeof(p.proj));
    p.radius = 1.5f;
    p.enable_blur = blur;
    p.blur_depth_threshold = 0.01f;
    p.blur_normal_threshold = 0.9f;
    return p;
}

compute::SSRParams ssrParams(std::vector<Vec4>& out, bool rough) {
    const Scene& s = scene();
    compute::SSRParams p{};
    p.depth_surface = const_cast<f32*>(s.depth.data());
    p.normal_surface = const_cast<Vec3*>(s.normals.data());
    p.scene_color_surface = const_cast<Vec3*>(s.color.data());
    p.roughness_surface = rough ? const_cast<f32*>(s.roughness.data()) : nullptr;
    p.ssr_out_surface = out.data();
    p.width = kW;
    p.height = kH;
    std::memcpy(p.proj, s.proj, sizeof(p.proj));
    return p;
}

compute::SSGIParams ssgiParams(std::vector<Vec3>& out, u32 bounces) {
    const Scene& s = scene();
    compute::SSGIParams p{};
    p.depth_surface = const_cast<f32*>(s.depth.data());
    p.normal_surface = const_cast<Vec3*>(s.normals.data());
    p.scene_color_surface = const_cast<Vec3*>(s.color.data());
    p.albedo_surface = const_cast<Vec3*>(s.albedo.data());
    p.ssgi_out_surface = out.data();
    p.width = kW;
    p.height = kH;
    std::memcpy(p.proj, s.proj, sizeof(p.proj));
    p.sample_sqrt = 3;
    p.max_bounces = bounces;
    return p;
}

/// Every surface one backend produces (poisoned before each launch so unwritten pixels show up).
struct Frame {
    std::vector<f32> aoBlur = std::vector<f32>(kPixels, -9.f);
    std::vector<f32> aoRaw = std::vector<f32>(kPixels, -9.f);
    std::vector<f32> aoRecon = std::vector<f32>(kPixels, -9.f);
    std::vector<Vec4> ssrMirror = std::vector<Vec4>(kPixels, Vec4{9.f, 9.f, 9.f, 9.f});
    std::vector<Vec4> ssrRough = std::vector<Vec4>(kPixels, Vec4{9.f, 9.f, 9.f, 9.f});
    std::vector<Vec3> ssgi1 = std::vector<Vec3>(kPixels, Vec3{9.f, 9.f, 9.f});
    std::vector<Vec3> ssgi3 = std::vector<Vec3>(kPixels, Vec3{9.f, 9.f, 9.f});
    // fuse_ssfx full-frame references (masked SSR with separate colour / confidence outputs).
    std::vector<f32> ssfxAo = std::vector<f32>(kPixels, -9.f);
    std::vector<Vec3> ssfxSsrColor = std::vector<Vec3>(kPixels, Vec3{9.f, 9.f, 9.f});
    std::vector<f32> ssfxSsrConfidence = std::vector<f32>(kPixels, -9.f);
    std::vector<Vec3> ssfxSsgi = std::vector<Vec3>(kPixels, Vec3{9.f, 9.f, 9.f});
};

ssfx::SsfxGBufferView ssfxView() {
    ssfx::SsfxGBufferView view{};
    const Scene& s = scene();
    compute::screen_space_kernels::make_view(s.proj, kW, kH, s.depth.data(), s.normals.data(), view);
    return view;
}

std::vector<f32> reflectiveMask() {
    std::vector<f32> mask(kPixels);
    for (u32 i = 0; i < kPixels; ++i) {
        mask[i] = (i % 5u) == 0u ? 0.f : 1.f;
    }
    return mask;
}

Frame render(kernel::Backend backend) {
    Frame f;
    expectTrue(compute::launch_ssao_cpu_backend(backend, ssaoParams(f.aoBlur, true, true)), "ssao (blur) launch");
    expectTrue(compute::launch_ssao_cpu_backend(backend, ssaoParams(f.aoRaw, false, true)), "ssao (raw) launch");
    expectTrue(compute::launch_ssao_cpu_backend(backend, ssaoParams(f.aoRecon, true, false)),
               "ssao (reconstructed normals) launch");
    expectTrue(compute::launch_ssr_cpu_backend(backend, ssrParams(f.ssrMirror, false)), "ssr (mirror) launch");
    expectTrue(compute::launch_ssr_cpu_backend(backend, ssrParams(f.ssrRough, true)), "ssr (roughness) launch");
    expectTrue(compute::launch_ssgi_cpu_backend(backend, ssgiParams(f.ssgi1, 1)), "ssgi (1 bounce) launch");
    expectTrue(compute::launch_ssgi_cpu_backend(backend, ssgiParams(f.ssgi3, 3)), "ssgi (3 bounces) launch");

    const ssfx::SsfxGBufferView view = ssfxView();
    const std::vector<f32> mask = reflectiveMask();
    ssfx::HbaoParams hbao{};
    hbao.radius = 1.2f;
    hbao.directions = 12;
    expectTrue(ssfx::computeHbaoCpu(view, hbao, f.ssfxAo.data(), backend), "ssfx computeHbaoCpu");
    expectTrue(ssfx::computeSsrCpu(view, scene().color.data(), ssfx::SsrParams{}, mask.data(), f.ssfxSsrColor.data(),
                                   f.ssfxSsrConfidence.data(), backend),
               "ssfx computeSsrCpu");
    ssfx::SsgiParams ssgi{};
    ssgi.sample_sqrt = 2;
    ssgi.bounces = 2;
    expectTrue(ssfx::computeSsgiCpu(view, scene().color.data(), nullptr, ssgi, f.ssfxSsgi.data(), backend),
               "ssfx computeSsgiCpu");
    return f;
}

template <typename T>
void mergeBitwise(kernel::ParityReport& report, const std::vector<T>& a, const std::vector<T>& b) {
    report.merge(kernel::compare_bitwise(std::span<const T>(a), std::span<const T>(b)));
}

kernel::ParityReport compareFrames(const Frame& a, const Frame& b) {
    kernel::ParityReport report = kernel::compare_bitwise(std::span<const f32>(a.aoBlur), std::span<const f32>(b.aoBlur));
    mergeBitwise(report, a.aoRaw, b.aoRaw);
    mergeBitwise(report, a.aoRecon, b.aoRecon);
    mergeBitwise(report, a.ssrMirror, b.ssrMirror);
    mergeBitwise(report, a.ssrRough, b.ssrRough);
    mergeBitwise(report, a.ssgi1, b.ssgi1);
    mergeBitwise(report, a.ssgi3, b.ssgi3);
    mergeBitwise(report, a.ssfxAo, b.ssfxAo);
    mergeBitwise(report, a.ssfxSsrColor, b.ssfxSsrColor);
    mergeBitwise(report, a.ssfxSsrConfidence, b.ssfxSsrConfidence);
    mergeBitwise(report, a.ssfxSsgi, b.ssfxSsgi);
    return report;
}

constexpr u64 kSurfaces = 11;

void testBackendParity() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    const Frame reference = render(kernel::Backend::CpuReference);

    // The frame exercises the interesting paths (occlusion, blur, hits, gloss fade, indirect light, sky).
    u32 occluded = 0;
    u32 blurred = 0;
    u32 hits = 0;
    u32 faded = 0;
    u32 lit = 0;
    u32 bounceGain = 0;
    u32 sky = 0;
    for (u32 i = 0; i < kPixels; ++i) {
        occluded += reference.aoRaw[i] < 0.9f ? 1u : 0u;
        blurred += reference.aoRaw[i] != reference.aoBlur[i] ? 1u : 0u;
        hits += reference.ssrMirror[i].w > 0.f ? 1u : 0u;
        faded += reference.ssrRough[i].w < reference.ssrMirror[i].w ? 1u : 0u;
        lit += reference.ssgi1[i].y > 0.f ? 1u : 0u;
        bounceGain += reference.ssgi3[i].y > reference.ssgi1[i].y ? 1u : 0u;
        sky += scene().depth[i] <= 0.f ? 1u : 0u;
    }
    expectTrue(occluded > 20u && blurred > 100u && hits > 200u && faded > 100u && lit > 200u && bounceGain > 50u &&
                   sky > 100u,
               "parity frame covers occlusion, blur, SSR hits, gloss fade, indirect light, bounces and sky");

    for (u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        const Frame parallel = render(kernel::Backend::CpuParallel);
        const kernel::ParityReport report = compareFrames(reference, parallel);
        char label[160];
        std::snprintf(label, sizeof(label),
                      "CpuReference == CpuParallel bit-exact SSAO/SSR/SSGI surfaces (%u workers, %llu mismatches)",
                      workers, static_cast<unsigned long long>(report.mismatches));
        expectTrue(report.ok && report.compared == kPixels * kSurfaces, label);
    }

    // run_parity on the raw kernels (the harness path other ports use).
    std::vector<f32> a(kPixels, -1.f);
    std::vector<f32> b(kPixels, -2.f);
    const ssfx::SsfxGBufferView view = ssfxView();
    const ssfx::HbaoParams hbao{};
    const kernel::ParityReport hbaoReport = kernel::run_parity(
        kernel::Backend::CpuReference, kernel::Backend::CpuParallel, ssfx::hbao_kernel::make_launch(view),
        ssfx::hbao_kernel::Kernel{}, ssfx::hbao_kernel::make_params(view, hbao, a.data()),
        ssfx::hbao_kernel::make_params(view, hbao, b.data()),
        [&] { return kernel::compare_bitwise(std::span<const f32>(a), std::span<const f32>(b)); });
    expectTrue(hbaoReport.ok && hbaoReport.backend_b == kernel::Backend::CpuParallel,
               "run_parity: hbao_kernel CpuReference == CpuParallel");

    // One implementation: kernel pixels == the public scalar API.
    bool same = true;
    const ssfx::SsrParams ssr{};
    for (u32 y = 0; y < kH; y += 3) {
        for (u32 x = 0; x < kW; x += 2) {
            const u32 i = y * kW + x;
            same = same && ssfx::hbaoPixelVisibility(view, hbao, x, y) == a[i];
            const ssfx::SsrHit hit = ssfx::ssrTracePixel(view, scene().color.data(), ssr, x, y);
            const bool masked = (i % 5u) == 0u;
            same = same && (masked ? reference.ssfxSsrConfidence[i] == 0.f
                                   : reference.ssfxSsrConfidence[i] == (hit.hit ? hit.confidence : 0.f));
        }
    }
    expectTrue(same, "kernel pixels == hbaoPixelVisibility / ssrTracePixel");

    // SSGI output aliasing an input (albedo) renders through a temporary: same result as distinct buffers.
    std::vector<Vec3> aliased = scene().albedo;
    compute::SSGIParams aliasParams = ssgiParams(aliased, 3);
    aliasParams.albedo_surface = aliased.data();
    expectTrue(compute::launch_ssgi_cpu(aliasParams) &&
                   kernel::compare_bitwise(std::span<const Vec3>(aliased), std::span<const Vec3>(reference.ssgi3)).ok,
               "SSGI output aliasing the albedo input == distinct output");
    scheduler.shutdown();
}

bool statsMatch(const char* name, u64 launches, kernel::Backend backend) {
    kernel::KernelStats stats{};
    const u64 groups = ((kW + 7u) / 8u) * ((kH + 7u) / 8u);
    return kernel::find_kernel_stats(name, stats) && stats.launches == launches && stats.failed_launches == 0u &&
           stats.items == launches * kPixels && stats.workgroups == launches * groups && stats.last_backend == backend;
}

void testStatsAndFallback() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    Frame ref;
    kernel::reset_kernel_stats();
    expectTrue(compute::launch_ssao_on(kernel::Backend::CpuReference, ssaoParams(ref.aoBlur, true, true)),
               "ssao reference");
    expectTrue(statsMatch("screen_space_ao", 1u, kernel::Backend::CpuReference) &&
                   statsMatch("screen_space_ao_blur", 1u, kernel::Backend::CpuReference),
               "SSAO records screen_space_ao + screen_space_ao_blur (items, 8x8 workgroups, backend)");
    expectTrue(compute::launch_ssr_on(kernel::Backend::CpuReference, ssrParams(ref.ssrRough, true)), "ssr reference");
    expectTrue(statsMatch("screen_space_reflections", 1u, kernel::Backend::CpuReference),
               "SSR records screen_space_reflections");
    expectTrue(compute::launch_ssgi_on(kernel::Backend::CpuReference, ssgiParams(ref.ssgi3, 3)), "ssgi reference");
    expectTrue(statsMatch("screen_space_gi", 3u, kernel::Backend::CpuReference),
               "SSGI records one screen_space_gi launch per bounce");
    std::vector<Vec3> zero(kPixels, Vec3{9.f, 9.f, 9.f});
    expectTrue(compute::launch_ssgi_cpu(ssgiParams(zero, 0)) && statsMatch("screen_space_gi", 3u,
                                                                          kernel::Backend::CpuReference),
               "SSGI with 0 bounces launches nothing");
    bool allZero = true;
    for (const Vec3& v : zero) {
        allZero = allZero && v.x == 0.f && v.y == 0.f && v.z == 0.f;
    }
    expectTrue(allZero, "SSGI with 0 bounces writes zero indirect light");
    expectTrue(compute::ssao_center_sample(ssaoParams(ref.aoRaw, false, true)) ==
                   ssfx::hbaoPixelVisibility(ssfxView(), compute::screen_space_kernels::to_hbao(
                                                             ssaoParams(ref.aoRaw, false, true)),
                                             kW / 2u, kH / 2u),
               "ssao_center_sample == hbao scalar API");

    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        expectTrue(!compute::screen_space_effects_info().device_available, "screen_space_effects_info: no device");
        for (kernel::Backend gpu : {kernel::Backend::Cuda, kernel::Backend::Auto, kernel::Backend::VulkanCompute}) {
            Frame fb;
            expectTrue(compute::launch_ssao_on(gpu, ssaoParams(fb.aoBlur, true, true)), "ssao GPU request succeeds");
            kernel::LaunchRecord last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel &&
                           std::strcmp(last.name, "screen_space_ao_blur") == 0,
                       "SSAO GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(compute::launch_ssr_on(gpu, ssrParams(fb.ssrRough, true)), "ssr GPU request succeeds");
            last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel &&
                           std::strcmp(last.name, "screen_space_reflections") == 0,
                       "SSR GPU request without a device falls back to CpuParallel (recorded)");
            expectTrue(compute::launch_ssgi_on(gpu, ssgiParams(fb.ssgi3, 3)), "ssgi GPU request succeeds");
            last = kernel::last_launch();
            expectTrue(last.ok && last.requested == gpu && last.backend == kernel::Backend::CpuParallel &&
                           std::strcmp(last.name, "screen_space_gi") == 0,
                       "SSGI GPU request without a device falls back to CpuParallel (recorded)");
            kernel::ParityReport report =
                kernel::compare_bitwise(std::span<const f32>(ref.aoBlur), std::span<const f32>(fb.aoBlur));
            mergeBitwise(report, ref.ssrRough, fb.ssrRough);
            mergeBitwise(report, ref.ssgi3, fb.ssgi3);
            expectTrue(report.ok, "fallback surfaces == CpuReference surfaces");
        }
        Frame viaLaunch;
        expectTrue(compute::launch_ssao(ssaoParams(viaLaunch.aoBlur, true, true)) &&
                       compute::launch_ssr(ssrParams(viaLaunch.ssrRough, true)) &&
                       compute::launch_ssgi(ssgiParams(viaLaunch.ssgi3, 3)),
                   "launch_ssao / launch_ssr / launch_ssgi (Auto) succeed");
        kernel::ParityReport report =
            kernel::compare_bitwise(std::span<const f32>(ref.aoBlur), std::span<const f32>(viaLaunch.aoBlur));
        mergeBitwise(report, ref.ssrRough, viaLaunch.ssrRough);
        mergeBitwise(report, ref.ssgi3, viaLaunch.ssgi3);
        expectTrue(report.ok, "launch_* (Auto) surfaces == CpuReference surfaces");
    } else {
        // Device present: CUDA transcendentals differ from host libm by ulps, and a march can flip a hit at a
        // grazing sample — tolerance parity with a small mismatch budget.
        Frame gpu;
        expectTrue(compute::launch_ssao_on(kernel::Backend::Cuda, ssaoParams(gpu.aoBlur, true, true)) &&
                       compute::launch_ssr_on(kernel::Backend::Cuda, ssrParams(gpu.ssrRough, true)) &&
                       compute::launch_ssgi_on(kernel::Backend::Cuda, ssgiParams(gpu.ssgi3, 3)),
                   "CUDA launches succeed");
        const kernel::Tolerance tol{1e-3, 1e-3};
        expectTrue(kernel::compare_floats(std::span<const f32>(ref.aoBlur), std::span<const f32>(gpu.aoBlur), tol)
                           .mismatches <= kPixels / 100u,
                   "CUDA SSAO within tolerance of CpuReference");
        expectTrue(kernel::compare_floats(std::span<const Vec4>(ref.ssrRough), std::span<const Vec4>(gpu.ssrRough), tol)
                           .mismatches <= kPixels * 4u / 100u,
                   "CUDA SSR within tolerance of CpuReference");
        expectTrue(kernel::compare_floats(std::span<const Vec3>(ref.ssgi3), std::span<const Vec3>(gpu.ssgi3), tol)
                           .mismatches <= kPixels * 3u / 50u,
                   "CUDA SSGI within tolerance of CpuReference");
    }

    // Invalid params are rejected on every backend (and nothing is launched).
    std::vector<f32> ao(kPixels);
    std::vector<Vec4> ssrOut(kPixels);
    std::vector<Vec3> ssgiOut(kPixels);
    const u64 before = kernel::total_launch_count();
    for (kernel::Backend b : {kernel::Backend::CpuReference, kernel::Backend::CpuParallel, kernel::Backend::Cuda,
                              kernel::Backend::Auto}) {
        compute::SSAOParams badAo = ssaoParams(ao, true, true);
        badAo.proj[11] = 0.f; // not a perspective projection
        compute::SSRParams badSsr = ssrParams(ssrOut, true);
        badSsr.scene_color_surface = nullptr;
        compute::SSGIParams badSsgi = ssgiParams(ssgiOut, 1);
        badSsgi.sample_sqrt = 0;
        expectTrue(!compute::launch_ssao_on(b, badAo) && !compute::launch_ssr_on(b, badSsr) &&
                       !compute::launch_ssgi_on(b, badSsgi),
                   "invalid params rejected on every backend");
    }
    expectTrue(kernel::total_launch_count() == before, "rejected launches record nothing");
    scheduler.shutdown();
}

} // namespace

int main() {
    fuse::core::initialize();
    testBackendParity();
    testStatsAndFallback();
    fuse::core::shutdown();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d screen-space kernel parity check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("Screen-space (SSAO/SSR/SSGI) kernel parity gates passed\n");
    return EXIT_SUCCESS;
}
