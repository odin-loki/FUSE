// FUSE Relight RL-4.3 Lavapipe gate (VK_LAYER_KHRONOS_validation with synchronization validation; every validation
// message fails the run): the Relight BSDF on the GPU == the CPU reference (plan §5.8: GPU/CPU <= 1e-4).
//
// The probe kernel shaders/material/bsdf_probe.{slang,comp} runs bsdfEval, bsdfPdf and bsdfSample of the Slang
// implementation (bsdf.slang) or the GLSL fallback (bsdf.glsl) over every configuration of the CPU gates (random
// wo / wi / u, both hemispheres where the model is two-sided); the CPU side runs the bsdf_eval / bsdf_pdf /
// bsdf_sample kernels (kernel::Backend::CpuReference) on the same cases and the same albedo table.
//
//   eval, pdf, sample weight / pdf   |gpu - cpu| <= 1e-4 max(|gpu|, |cpu|) + 1e-6 per channel
//   sample direction                 |gpu - cpu| <= 1e-4 per component
//   sample flags (lobe, dirac, ...)  equal (a lobe choice that rounds differently on a selection boundary is
//                                    reported with its u.x and fails the gate; none occur on these cases)
//
// The tolerance covers the device's transcendental / division rounding (Vulkan: a few ulp; Lavapipe ~1 ulp) through
// the longest chains (hair: I0 series, logistic CDFs; thin film: three spectral terms).
//
//   --language slang | glsl
// Exit 77 = skip (stub build, no ICD / validation layer, kernel not built).

#include "bsdf_test_common.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include "rp_wp05_vk.hpp"

#include <vulkan/vulkan.h>
#endif

namespace {
constexpr int kSkip = 77;
} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace rl_bsdf_test;
namespace rm = fuse::relight::render::material;
using fuse::u32;

constexpr u32 kCaseWords = 14u; // float4 per case (bsdf_probe)
constexpr u32 kOutWords = 3u;
constexpr double kTolRel = 1e-4;
constexpr double kTolAbs = 1e-6;

std::vector<std::uint32_t> readSpirv(const char* path) {
    std::vector<std::uint32_t> words;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return words;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0 && size % 4 == 0) {
        words.resize(std::size_t(size) / 4u);
        if (std::fread(words.data(), 4u, words.size(), f) != words.size()) {
            words.clear();
        }
    }
    std::fclose(f);
    return words;
}

bool close(double gpu, double cpu) { return std::fabs(gpu - cpu) <= kTolRel * std::max(std::fabs(gpu), std::fabs(cpu)) + kTolAbs; }

} // namespace

int main(int argc, char** argv) {
    std::string language = "slang";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--language") == 0) {
            language = argv[i + 1];
        }
    }
    const char* spvPath = nullptr;
#if defined(FUSE_RL_BSDF_SLANG_SPV)
    if (language == "slang") {
        spvPath = FUSE_RL_BSDF_SLANG_SPV;
    }
#endif
#if defined(FUSE_RL_BSDF_GLSL_SPV)
    if (language == "glsl") {
        spvPath = FUSE_RL_BSDF_GLSL_SPV;
    }
#endif
    if (spvPath == nullptr) {
        std::printf("SKIP: %s probe kernel not built (compiler unavailable)\n", language.c_str());
        return kSkip;
    }
    const std::vector<std::uint32_t> spirv = readSpirv(spvPath);
    if (spirv.empty()) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", spvPath);
        return 1;
    }

    // Cases: every configuration, random directions and uniforms; the CPU reference on the same cases.
    rm::BsdfCases cases;
    Rng rng(0xb5dfu);
    for (const Config& c : allConfigs()) {
        for (int i = 0; i < 96; ++i) {
            float3 wo = rng.sphere();
            if (!c.twoSided) {
                wo.z = std::fabs(wo.z);
            }
            float3 wi = rng.sphere();
            if (i % 2 == 0 && !c.hair) {
                wi = float3(-wo.x, -wo.y, wo.z) * 0.98f + rng.sphere() * 0.2f; // near the specular peak
                wi = normalize(wi);
            }
            cases.add(c.m, wo, wi, rng.uniform4());
        }
    }
    const rm::AlbedoLut& lut = rm::sharedAlbedoLut();
    rm::BsdfResults cpu;
    if (!rm::runBsdfKernels(fuse::kernel::Backend::CpuReference, lut, cases, cpu)) {
        std::fprintf(stderr, "FAIL: CPU reference launch\n");
        return 1;
    }
    const u32 n = cases.count();

    rp_wp05::Context ctx;
    const int setup = rp_wp05::setupContext(ctx, "rl_bsdf_vk_parity");
    if (setup != 0) {
        return setup;
    }

    rp_wp05::HostBuffer caseBuf;
    rp_wp05::HostBuffer lutBuf;
    rp_wp05::HostBuffer outBuf;
    if (!caseBuf.create(ctx, VkDeviceSize(n) * kCaseWords * sizeof(float4)) ||
        !lutBuf.create(ctx, VkDeviceSize(bsdf::kBsdfLutWords) * sizeof(float)) ||
        !outBuf.create(ctx, VkDeviceSize(n) * kOutWords * sizeof(float4))) {
        std::fprintf(stderr, "FAIL: buffer allocation\n");
        return 1;
    }
    auto* cw = static_cast<float4*>(caseBuf.mapped);
    for (u32 i = 0; i < n; ++i) {
        bsdf::bsdfMaterialPack(cases.materials[i], cw + std::size_t(i) * kCaseWords);
        const float3& o = cases.wo[i];
        const float3& w = cases.wi[i];
        cw[std::size_t(i) * kCaseWords + 11u] = float4(o.x, o.y, o.z, 0.f);
        cw[std::size_t(i) * kCaseWords + 12u] = float4(w.x, w.y, w.z, 0.f);
        cw[std::size_t(i) * kCaseWords + 13u] = cases.u[i];
    }
    std::memcpy(lutBuf.mapped, lut.data(), std::size_t(bsdf::kBsdfLutWords) * sizeof(float));
    std::memset(outBuf.mapped, 0, std::size_t(n) * kOutWords * sizeof(float4));

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.size() * 4u;
    moduleInfo.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx.vkDevice, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: vkCreateShaderModule\n");
        return 1;
    }
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (u32 b = 0; b < 3u; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1u;
        bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 3u;
    setInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(ctx.vkDevice, &setInfo, nullptr, &setLayout);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1u;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1u;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(ctx.vkDevice, &layoutInfo, nullptr, &layout);
    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeInfo.stage.module = module;
    pipeInfo.stage.pName = "main";
    pipeInfo.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(ctx.vkDevice, VK_NULL_HANDLE, 1u, &pipeInfo, nullptr, &pipeline) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: vkCreateComputePipelines\n");
        return 1;
    }
    const u32 push[4] = {n, 0u, 0u, 0u};
    const bool dispatched = rp_wp05::dispatchCompute(ctx, pipeline, layout, setLayout,
                                                     {{0u, &caseBuf}, {1u, &lutBuf}, {2u, &outBuf}}, push, 16u,
                                                     (n + 63u) / 64u);
    vkDestroyPipeline(ctx.vkDevice, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.vkDevice, layout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.vkDevice, setLayout, nullptr);
    vkDestroyShaderModule(ctx.vkDevice, module, nullptr);
    if (!dispatched) {
        std::fprintf(stderr, "FAIL: dispatch\n");
        return 1;
    }

    const auto* out = static_cast<const float4*>(outBuf.mapped);
    int failures = 0;
    int ambiguous = 0;
    double worstRel = 0.0;
    auto rel = [](double g, double c) { return std::fabs(g - c) / std::max(std::max(std::fabs(g), std::fabs(c)), 1e-3); };
    const std::vector<Config> configs = allConfigs();
    for (u32 i = 0; i < n; ++i) {
        const float4 e = out[std::size_t(i) * kOutWords + 0u];
        const float4 s0 = out[std::size_t(i) * kOutWords + 1u];
        const float4 s1 = out[std::size_t(i) * kOutWords + 2u];
        const float3& ce = cpu.eval[i];
        const bsdf::BsdfSample& cs = cpu.samples[i];
        bool ok = close(e.x, ce.x) && close(e.y, ce.y) && close(e.z, ce.z) && close(e.w, cpu.pdf[i]);
        worstRel = std::max({worstRel, rel(e.x, ce.x), rel(e.y, ce.y), rel(e.z, ce.z), rel(e.w, cpu.pdf[i])});
        const auto gpuFlags = static_cast<bsdf::uint>(s1.w);
        if (gpuFlags != cs.flags) {
            ++ambiguous;
            std::fprintf(stderr, "flags differ (case %u, %s): gpu %u cpu %u, u.x = %.7f\n", i,
                         configs[i / 96u].name.c_str(), gpuFlags, cs.flags, cases.u[i].x);
            continue;
        }
        if ((cs.flags & bsdf::kBsdfSampleValid) != 0u) {
            ok = ok && std::fabs(s0.x - cs.wi.x) <= 1e-4 && std::fabs(s0.y - cs.wi.y) <= 1e-4 &&
                 std::fabs(s0.z - cs.wi.z) <= 1e-4 && close(s0.w, cs.pdf) && close(s1.x, cs.weight.x) &&
                 close(s1.y, cs.weight.y) && close(s1.z, cs.weight.z);
            worstRel = std::max({worstRel, rel(s0.w, cs.pdf), rel(s1.x, cs.weight.x), rel(s1.y, cs.weight.y),
                                 rel(s1.z, cs.weight.z)});
        }
        if (!ok) {
            if (failures < 20) {
                std::fprintf(stderr,
                             "FAIL: case %u (%s): gpu eval (%g %g %g) pdf %g | cpu (%g %g %g) %g; sample gpu wi (%g %g %g) "
                             "pdf %g w (%g %g %g) | cpu wi (%g %g %g) pdf %g w (%g %g %g)\n",
                             i, configs[i / 96u].name.c_str(), e.x, e.y, e.z, e.w, ce.x, ce.y, ce.z, cpu.pdf[i], s0.x, s0.y,
                             s0.z, s0.w, s1.x, s1.y, s1.z, cs.wi.x, cs.wi.y, cs.wi.z, cs.pdf, cs.weight.x, cs.weight.y,
                             cs.weight.z);
            }
            ++failures;
        }
    }
    const u32 messages = rp_wp05::validationMessageCount();
    std::printf("rl_bsdf_vk_parity (%s): %u cases, %d mismatches, %d flag differences, worst relative difference %.2e, "
                "%u validation messages\n",
                language.c_str(), n, failures, ambiguous, worstRel, messages);
    if (failures != 0 || ambiguous != 0 || messages != 0u) {
        return 1;
    }
    std::printf("OK\n");
    return 0;
}

#endif // FUSE_VULKAN_BACKEND
