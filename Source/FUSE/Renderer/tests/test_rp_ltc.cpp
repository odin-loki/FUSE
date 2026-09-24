// WP-2.2 Lavapipe gate (VK_LAYER_KHRONOS_validation with synchronization validation; every validation
// message fails the run): the per-light code of light.shade (shaders/lighting/lc_ltc.{glsl,slang}:
// fuse_lc_surface_terms + fuse_lc_light_ms, i.e. the compensated BRDF, LTC rectangle / disk area lights
// and sun disks, reading the BRDF LUT through BDA) run on the GPU by the probe kernel
// shaders/lighting/ltc/ltc_probe.{comp,slang} over a list of (surface, view, light) cases:
//
//   GPU == CPU   every case within kTolRel x |case| + kTolAbs of lighting_gpu::light_contribution (the
//                CPU reference of the same expressions; the justification is at kTolRel)
//   GPU vs MC    the GPU's area-light and sun results against the f64 Monte Carlo references of the CPU
//                gates (test_rp_ltc_mc.hpp): the same error budget as fuse_rp_ltc_{rect,disk,sun}
//
//   --language slang | glsl
//
// Exit 77 = skip (stub build, no ICD / validation layer, BDA missing, kernel not built).

#include "test_rp_ltc_mc.hpp"

#include <fuse/renderer/lighting/gpu/clustered_lighting.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include "rp_wp05_vk.hpp"

#include <vulkan/vulkan.h>
#endif

namespace {
constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace ltc_test;
using fuse::u64;
using fuse::u8;
using fuse::math::Vec4;

// GPU vs CPU: both evaluate the same f32 expressions in the same order without contraction (`precise` /
// -fp-mode precise). The differences are the device's rounding of sqrt, division (2.5 ulp),
// inversesqrt, atan2 / acos / cos (Vulkan: a few ulp or 2^-11 absolute for the trigonometric ones on
// Lavapipe's LLVM, in practice ~1 ulp): the bilinear LUT weights move by an ulp of the coordinate
// (sqrt(1 - N.V)), the edge integrals of a polygon cancel (a far light is a small difference of edge
// terms: relative error ~ ulp x distance / size, <= 100 x 1e-7 here), and the disk's boundary integral
// sums 64 trapezoid terms (its large mean removed analytically) or clips a 32-gon.
// kTolRel = 2e-4 of the case's largest channel (>= 10x the worst of these) + kTolAbs = 1e-7.
// Area lights and sun disks add an absolute term in form-factor units: a polygon's form factor is a sum
// of 4-5 edge terms of up to pi that cancel down to the (possibly tiny) result, so its absolute error is
// a few ulp of pi per edge (~1e-6) whatever the result; kTolFF = 1e-5 of the light's full response
// (radiance x window x (specular + diffuse albedo)), i.e. of a light filling the hemisphere.
constexpr f64 kTolRel = 2e-4;
constexpr f64 kTolAbs = 1e-7;
constexpr f64 kTolFF = 1e-5;

// 128 bytes (ltc_probe FuseLtcProbeCase).
struct ProbeCase {
    gpu_scene::GpuLight light{};
    f32 position[3] = {};
    f32 roughness = 0.5f;
    f32 normal[3] = {0.f, 0.f, 1.f};
    f32 metallic = 0.f;
    f32 albedo[3] = {1.f, 1.f, 1.f};
    f32 pad0 = 0.f;
    f32 view[3] = {0.f, 0.f, 1.f};
    f32 pad1 = 0.f;
};
static_assert(sizeof(ProbeCase) == 128u, "ProbeCase layout (ltc_probe)");

struct ProbePush {
    u64 cases = 0;
    u64 lut = 0;
    u64 out = 0;
    u32 count = 0;
    u32 pad = 0;
};
static_assert(sizeof(ProbePush) == 32u, "ProbePush layout");

enum class Kind : u8 { Rect, RectStraddle, Disk, DiskStraddle, Sun, SunGrazing, Point, Spot, Directional };
const char* kindName(Kind k) {
    switch (k) {
    case Kind::Rect: return "rect";
    case Kind::RectStraddle: return "rect/horizon";
    case Kind::Disk: return "disk";
    case Kind::DiskStraddle: return "disk/horizon";
    case Kind::Sun: return "sun";
    case Kind::SunGrazing: return "sun/horizon";
    case Kind::Point: return "point";
    case Kind::Spot: return "spot";
    default: return "directional";
    }
}
constexpr u32 kKinds = 9;

struct Case {
    Kind kind = Kind::Rect;
    SurfaceSample s{};
    Vec3 v{};
    gpu_scene::GpuLight light{};
    f32 sunRadius = 0.f;
    Vec3 sunDir{};
};

std::vector<Case> makeCases() {
    std::vector<Case> cases;
    std::mt19937 rng(77);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    for (u32 k = 0; k < 36u; ++k) {
        for (u32 type : {ltc::kLightRect, ltc::kLightDisk}) {
            for (bool straddle : {false, true}) {
                const Config c = makeConfig(rng, k, type, straddle);
                Case cs{};
                cs.kind = type == ltc::kLightRect ? (straddle ? Kind::RectStraddle : Kind::Rect)
                                                   : (straddle ? Kind::DiskStraddle : Kind::Disk);
                cs.s = c.s;
                cs.v = c.v;
                cs.light = c.light;
                cases.push_back(cs);
            }
        }
    }
    // Suns, points, spots, punctual directional lights on random surfaces.
    for (u32 k = 0; k < 48u; ++k) {
        const Config c = makeConfig(rng, k, ltc::kLightRect, false);
        const Frame fr = frameOf(d3(c.s.normal), d3(c.v));
        const bool graze = k % 4u == 3u;
        const f32 radius = k % 2u == 0u ? 0.0047f : 0.052f;
        const f64 elev = graze ? static_cast<f64>(radius) * (2.0 * u(rng) - 1.0) : 0.2 + 1.3 * u(rng);
        const f64 ph = 2.0 * ltc_test::kPi * u(rng);
        const D3 l = fr.toWorld({std::cos(elev) * std::cos(ph), std::cos(elev) * std::sin(ph), std::sin(elev)});
        Case sun{};
        sun.kind = graze ? Kind::SunGrazing : Kind::Sun;
        sun.s = c.s;
        sun.v = c.v;
        sun.light = directionalOf(l);
        ltc::setSunAngularRadius(sun.light, radius);
        sun.sunRadius = radius;
        sun.sunDir = f3(l);
        cases.push_back(sun);
        Case dir = sun;
        dir.kind = Kind::Directional;
        dir.light = directionalOf(l);
        cases.push_back(dir);
        Case point{};
        point.kind = k % 2u == 0u ? Kind::Point : Kind::Spot;
        point.s = c.s;
        point.v = c.v;
        point.light = c.light;
        point.light.type = static_cast<u32>(point.kind == Kind::Point ? gpu_scene::GpuLightType::Point : gpu_scene::GpuLightType::Spot);
        point.light.direction[0] = static_cast<f32>(-l.x);
        point.light.direction[1] = static_cast<f32>(-l.y);
        point.light.direction[2] = static_cast<f32>(-l.z);
        point.light.position[0] = c.s.position.x + static_cast<f32>(l.x) * 2.f;
        point.light.position[1] = c.s.position.y + static_cast<f32>(l.y) * 2.f;
        point.light.position[2] = c.s.position.z + static_cast<f32>(l.z) * 2.f;
        point.light.cosInner = 0.99f;
        point.light.cosOuter = 0.9f;
        point.light.range = 5.f;
        cases.push_back(point);
    }
    return cases;
}

ProbeCase toProbe(const Case& c) {
    ProbeCase p{};
    p.light = c.light;
    p.position[0] = c.s.position.x;
    p.position[1] = c.s.position.y;
    p.position[2] = c.s.position.z;
    p.roughness = c.s.roughness;
    p.normal[0] = c.s.normal.x;
    p.normal[1] = c.s.normal.y;
    p.normal[2] = c.s.normal.z;
    p.metallic = c.s.metallic;
    p.albedo[0] = c.s.albedo.x;
    p.albedo[1] = c.s.albedo.y;
    p.albedo[2] = c.s.albedo.z;
    p.view[0] = c.v.x;
    p.view[1] = c.v.y;
    p.view[2] = c.v.z;
    return p;
}

bool readFile(const char* path, std::vector<u32>& words) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    words.resize(static_cast<usize>(size > 0 ? size : 0) / 4u);
    const usize got = std::fread(words.data(), 4u, words.size(), f);
    std::fclose(f);
    return got == words.size() && !words.empty();
}

int run(const std::string& language) {
    const char* spvPath = nullptr;
#if defined(FUSE_LTC_PROBE_SLANG_SPV)
    if (language == "slang") {
        spvPath = FUSE_LTC_PROBE_SLANG_SPV;
    }
#endif
#if defined(FUSE_LTC_PROBE_GLSL_SPV)
    if (language == "glsl") {
        spvPath = FUSE_LTC_PROBE_GLSL_SPV;
    }
#endif
    if (spvPath == nullptr) {
        std::printf("SKIP: no %s probe kernel built\n", language.c_str());
        return kSkip;
    }
    std::vector<u32> spv;
    if (!readFile(spvPath, spv)) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", spvPath);
        return 1;
    }
    rp_wp05::Context ctx;
    const int setup = rp_wp05::setupContext(ctx, "fuse_rp_ltc");
    if (setup != 0) {
        return setup;
    }
    if (!lighting_gpu::queryLightingCapabilities(ctx.device.get()).lighting) {
        std::printf("SKIP: bufferDeviceAddress / shaderInt64 unsupported\n");
        return kSkip;
    }
    std::printf("device: %s, language %s\n", ctx.device->info().deviceName.c_str(), language.c_str());
    int rc = 0;
    {
        std::unique_ptr<GpuAllocator> allocator = GpuAllocator::create(*ctx.device);
        if (allocator == nullptr || !allocator->isValid()) {
            std::fprintf(stderr, "FAIL: GpuAllocator\n");
            return 1;
        }
        const std::vector<Case> cases = makeCases();
        const u32 count = static_cast<u32>(cases.size());
        const ltc::BrdfLut& table = ltc::sharedBrdfLut();
        auto makeBuffer = [&](usize bytes, MemoryUsage mem, const char* name, Buffer& out) {
            BufferDesc d{};
            d.size = bytes;
            d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) | static_cast<u32>(BufferUsage::ShaderDeviceAddress));
            d.memoryUsage = mem;
            d.name = name;
            return allocator->createBuffer(d, out) && out.mapped != nullptr && out.deviceAddress != 0u;
        };
        Buffer caseBuf{}, lutBuf{}, outBuf{};
        const bool buffersOk = makeBuffer(count * sizeof(ProbeCase), MemoryUsage::CpuToGpu, "ltc_probe.cases", caseBuf) &&
                               makeBuffer(static_cast<usize>(table.bytes()), MemoryUsage::CpuToGpu, "ltc_probe.lut", lutBuf) &&
                               makeBuffer(count * sizeof(Vec4), MemoryUsage::GpuToCpu, "ltc_probe.out", outBuf);
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (buffersOk) {
            std::vector<ProbeCase> probe(count);
            for (u32 i = 0; i < count; ++i) {
                probe[i] = toProbe(cases[i]);
            }
            std::memcpy(caseBuf.mapped, probe.data(), probe.size() * sizeof(ProbeCase));
            std::memcpy(lutBuf.mapped, table.data(), static_cast<usize>(table.bytes()));
            std::memset(outBuf.mapped, 0xFF, count * sizeof(Vec4));
            VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ProbePush)};
            VkPipelineLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            li.pushConstantRangeCount = 1;
            li.pPushConstantRanges = &range;
            VkShaderModuleCreateInfo mi{};
            mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            mi.codeSize = spv.size() * 4u;
            mi.pCode = spv.data();
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreatePipelineLayout(ctx.vkDevice, &li, nullptr, &layout) == VK_SUCCESS &&
                vkCreateShaderModule(ctx.vkDevice, &mi, nullptr, &module) == VK_SUCCESS) {
                VkComputePipelineCreateInfo pi{};
                pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                pi.stage.module = module;
                pi.stage.pName = "main";
                pi.layout = layout;
                vkCreateComputePipelines(ctx.vkDevice, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
                vkDestroyShaderModule(ctx.vkDevice, module, nullptr);
            }
        }
        if (!buffersOk || pipeline == VK_NULL_HANDLE) {
            std::fprintf(stderr, "FAIL: buffers / pipeline\n");
            rc = 1;
        } else {
            ProbePush push{};
            push.cases = caseBuf.deviceAddress;
            push.lut = lutBuf.deviceAddress;
            push.out = outBuf.deviceAddress;
            push.count = count;
            // Push-constant-only pipeline (no descriptor sets): record, submit and wait here.
            VkCommandPoolCreateInfo cpi{};
            cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            cpi.queueFamilyIndex = ctx.queueFamily;
            VkCommandPool pool = VK_NULL_HANDLE;
            vkCreateCommandPool(ctx.vkDevice, &cpi, nullptr, &pool);
            VkCommandBufferAllocateInfo cai{};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool = pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vkAllocateCommandBuffers(ctx.vkDevice, &cai, &cmd);
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (count + 63u) / 64u, 1, 1);
            VkMemoryBarrier2 toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &toHost;
            vkCmdPipelineBarrier2(cmd, &dep);
            vkEndCommandBuffer(cmd);
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence = VK_NULL_HANDLE;
            vkCreateFence(ctx.vkDevice, &fi, nullptr, &fence);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            const bool ok = vkQueueSubmit(ctx.queue, 1, &si, fence) == VK_SUCCESS &&
                            vkWaitForFences(ctx.vkDevice, 1, &fence, VK_TRUE, 120ull * 1000000000ull) == VK_SUCCESS;
            vkDestroyFence(ctx.vkDevice, fence, nullptr);
            vkDestroyCommandPool(ctx.vkDevice, pool, nullptr);
            expect(ok, "probe dispatch");

            std::vector<Vec4> gpu(count);
            std::memcpy(gpu.data(), outBuf.mapped, count * sizeof(Vec4));
            // GPU == CPU.
            f64 maxRel[kKinds] = {};
            f64 maxFull[kKinds] = {}; ///< max |gpu - cpu| / the light's full response
            u32 perKind[kKinds] = {};
            u32 bad = 0;
            u32 nonzero = 0;
            Stats mcArea_[2], mcSun_[2];
            for (u32 i = 0; i < count; ++i) {
                const Case& c = cases[i];
                const SurfaceTerms t = surface_terms(c.s, c.v, table.data());
                const Vec3 cpu = light_contribution(c.light, c.s, c.v, t, table.data());
                const f32 g[3] = {gpu[i].x, gpu[i].y, gpu[i].z};
                const f32 r[3] = {cpu.x, cpu.y, cpu.z};
                const f64 mag = std::max({std::fabs(static_cast<f64>(r[0])), std::fabs(static_cast<f64>(r[1])), std::fabs(static_cast<f64>(r[2]))});
                // Full response of the light (area lights: radiance x window; suns: irradiance / (pi sin^2)).
                f64 full = 0.0;
                if (c.light.type == ltc::kLightRect || c.light.type == ltc::kLightDisk) {
                    const Vec3 cc = Vec3{c.light.position[0], c.light.position[1], c.light.position[2]} - c.s.position;
                    full = c.light.intensity * ltc::area_window(cc.length(), c.light.range);
                } else if (c.kind == Kind::Sun || c.kind == Kind::SunGrazing) {
                    full = c.light.intensity / (ltc_test::kPi * std::sin(c.sunRadius) * std::sin(c.sunRadius));
                }
                full *= std::max({t.ms.specular.x + t.ms.diffuse.x, t.ms.specular.y + t.ms.diffuse.y, t.ms.specular.z + t.ms.diffuse.z});
                const u32 k = static_cast<u32>(c.kind);
                ++perKind[k];
                nonzero += mag > 0.0 ? 1u : 0u;
                bool caseBad = false;
                for (u32 ch = 0; ch < 3u; ++ch) {
                    const f64 diff = std::fabs(static_cast<f64>(g[ch]) - r[ch]);
                    if (mag > 0.0) {
                        maxRel[k] = std::max(maxRel[k], diff / mag);
                    }
                    if (full > 0.0) {
                        maxFull[k] = std::max(maxFull[k], diff / full);
                    }
                    caseBad = caseBad || !(diff <= kTolRel * mag + kTolFF * full + kTolAbs);
                }
                if (caseBad) {
                    ++bad;
                    if (bad <= 5u) {
                        std::fprintf(stderr, "  case %u (%s): gpu (%g %g %g) cpu (%g %g %g)\n", i, kindName(c.kind), g[0], g[1], g[2],
                                     r[0], r[1], r[2]);
                    }
                }
                // GPU vs Monte Carlo (red channel; the MC helpers use colour-1 lights).
                if (c.kind == Kind::Rect || c.kind == Kind::Disk || c.kind == Kind::RectStraddle || c.kind == Kind::DiskStraddle) {
                    Config cfg{};
                    cfg.s = c.s;
                    cfg.v = c.v;
                    cfg.light = c.light;
                    const f64 ref = mcArea(cfg, Part::Total);
                    const f64 floor = lightScale(cfg) * (t.ms.specular.x + t.ms.diffuse.x);
                    if (floor > 0.0) {
                        // The CPU gates' metric: error relative to the reference, floored at the light's
                        // brightness through the surface's albedo (tails are judged against the light).
                        const bool straddle = c.kind == Kind::RectStraddle || c.kind == Kind::DiskStraddle;
                        mcArea_[straddle ? 1 : 0].rel.push_back(std::fabs(g[0] - ref) / std::max(ref, floor));
                    }
                } else if (c.kind == Kind::Sun || c.kind == Kind::SunGrazing) {
                    const f64 ref = mcSun(c.s, c.v, c.sunDir, c.sunRadius, t);
                    const f64 floor = 0.1 * (t.ms.diffuse.x + t.ms.specular.x) / ltc_test::kPi;
                    mcSun_[c.kind == Kind::SunGrazing ? 1 : 0].rel.push_back(std::fabs(g[0] - ref) / std::max(ref, floor));
                }
            }
            std::printf("GPU vs CPU: %u cases (%u non-zero), %u outside kTolRel x |case| + kTolFF x full + kTolAbs; max relative per kind:\n", count,
                        nonzero, bad);
            for (u32 k = 0; k < kKinds; ++k) {
                std::printf("  %-12s %3u cases  %.2e  (x full response: %.2e)\n", kindName(static_cast<Kind>(k)), perKind[k], maxRel[k],
                            maxFull[k]);
            }
            std::printf("GPU vs Monte Carlo: area above the horizon mean %.2e p90 %.2e | straddling mean %.2e p90 %.2e\n",
                        mcArea_[0].mean(), mcArea_[0].percentile(0.9), mcArea_[1].mean(), mcArea_[1].percentile(0.9));
            std::printf("                    sun above mean %.2e p90 %.2e | grazing mean %.2e\n", mcSun_[0].mean(),
                        mcSun_[0].percentile(0.9), mcSun_[1].mean());
            expect(bad == 0u && nonzero > count / 2u, "GPU == CPU reference within kTolRel x |case| + kTolFF x full + kTolAbs");
            expect(mcArea_[0].mean() <= 0.05 && mcArea_[1].mean() <= 0.125, "GPU area lights vs Monte Carlo (the CPU gates' budget)");
            expect(mcSun_[0].mean() <= 0.05 && mcSun_[1].mean() <= 0.075, "GPU sun disks vs Monte Carlo (the CPU gates' budget)");
        }
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(ctx.vkDevice, pipeline, nullptr);
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx.vkDevice, layout, nullptr);
        }
        for (Buffer* b : {&caseBuf, &lutBuf, &outBuf}) {
            if (b->handle != nullptr) {
                allocator->destroyBuffer(*b);
            }
        }
    }
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    std::string language = "slang";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--language") == 0) {
            language = argv[i + 1];
        }
    }
    const int rc = run(language);
    if (rc == kSkip) {
        return kSkip;
    }
    const fuse::u32 messages = rp_wp05::validationMessageCount();
    std::printf("validation messages: %u\n", messages);
    if (rc != 0 || g_failures != 0 || messages != 0u) {
        std::fprintf(stderr, "FAIL: %d failure(s), %u validation message(s)\n", g_failures + rc, messages);
        return 1;
    }
    std::printf("PASS ltc probe (%s)\n", language.c_str());
    return 0;
}

#endif
