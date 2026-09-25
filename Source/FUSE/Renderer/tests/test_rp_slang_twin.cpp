// WP-0.5 Slang gates (docs/unification/RENDERER-EXECUTION.md §5 WP-0.5, §7 rp_slang_twin).
//
//   --mode twin        Build-time Slang kernels (fuse_add_slang_shaders) and their GLSL twins run on
//                      the device under validation + synchronization validation: the radix histogram
//                      (Slang twin of shaders/compute/radix_histogram.comp) and a float kernel must
//                      produce bit-identical buffers. Pipeline layouts come from SPIR-V reflection
//                      (identical for both languages). Runtime Slang compiles: permutation key (defines
//                      + tier) and background compile on the JobScheduler. 0 validation messages.
//   --mode spirv-val   spirv-val (Vulkan 1.3 env) accepts the build-time and runtime Slang SPIR-V.
//   --mode hot-reload  ShaderCompiler watches a Slang kernel that imports a module and #includes a
//                      file; editing either triggers exactly one recompile and the dispatched result
//                      changes accordingly. Same for a GLSL #include (glslangValidator depfile).
//
// Exit 77 = skip: stub build, Slang unavailable (FUSE_SLANG=OFF / no slangc), no ICD or layer.
#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/shader/shader_compiler.hpp>
#include <fuse/renderer/shader/shader_io.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/shader/shader_reflection.hpp>
#include <fuse/renderer/vk/compute_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>

#include "rp_wp05_vk.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int kSkipCode = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

[[maybe_unused]] int finish(const char* mode) {
    if (g_failures == 0) {
        std::printf("fuse_rp_slang_twin --mode %s: all checks passed\n", mode);
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_rp_slang_twin --mode %s: %d failure(s)\n", mode, g_failures);
    return EXIT_FAILURE;
}

} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkipCode;
}

#else

namespace {

using namespace fuse::renderer;
using fuse::u32;
using fuse::u64;
using rp_wp05::BufferBinding;
using rp_wp05::Context;
using rp_wp05::HostBuffer;

constexpr u32 kDescStorageBuffer = 7; // VK_DESCRIPTOR_TYPE_STORAGE_BUFFER

std::vector<u32> loadWords(const char* path) {
    std::string error;
    std::vector<u32> words = loadSpirvFile(path, &error);
    if (words.empty()) {
        std::fprintf(stderr, "cannot load %s: %s\n", path, error.c_str());
    }
    return words;
}

/// A compute pipeline whose layout comes from reflecting `words`.
struct ReflectedKernel {
    ShaderReflection reflection;
    std::unique_ptr<ShaderModule> module;
    std::unique_ptr<PipelineLayout> layout;
    std::unique_ptr<ComputePipeline> pipeline;

    bool build(VulkanDevice& device, const std::vector<u32>& words, const char* name) {
        reflection = reflectSpirv(words.data(), words.size());
        if (!reflection.valid) {
            std::fprintf(stderr, "%s: reflection failed: %s\n", name, reflection.message.c_str());
            return false;
        }
        module = ShaderModule::create(device, ShaderStage::Compute, words.data(), static_cast<u32>(words.size()));
        layout = PipelineLayout::createFromReflection(device, reflection, name);
        if (!module || !module->isValid() || !layout || !layout->isValid()) {
            std::fprintf(stderr, "%s: module/layout failed: %s\n", name, layout ? layout->info().message.c_str() : "");
            return false;
        }
        ComputePipelineDesc desc{};
        desc.layout = layout.get();
        desc.computeShader = module.get();
        desc.localSizeX = reflection.localSize[0];
        desc.debugName = name;
        pipeline = ComputePipeline::create(device, desc);
        return pipeline && pipeline->isValid();
    }

    bool run(const Context& ctx, const std::vector<BufferBinding>& bindings, const void* push, u32 groups) const {
        return rp_wp05::dispatchCompute(ctx, static_cast<VkPipeline>(pipeline->nativeHandle()),
                                        static_cast<VkPipelineLayout>(layout->nativeHandle()),
                                        static_cast<VkDescriptorSetLayout>(layout->setLayoutHandle(0)), bindings,
                                        push, reflection.pushConstantBytes, groups);
    }
};

u32 lcg(u32& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

#if defined(FUSE_RP_SLANG_TWIN_BUILT)

void checkHistogramReflection(const ShaderReflection& r, const char* what) {
    const std::string prefix(what);
    expect(r.valid, (prefix + ": reflection valid").c_str());
    expect(r.stageFlags == 0x20u, (prefix + ": compute stage").c_str());
    expect(r.localSize[0] == 256u && r.localSize[1] == 1u && r.localSize[2] == 1u,
           (prefix + ": local size 256x1x1").c_str());
    expect(r.pushConstantBytes == 24u, (prefix + ": 24-byte push block").c_str());
    const ShaderBindingReflection* keys = r.find(0, 0);
    const ShaderBindingReflection* scratch = r.find(0, 4);
    expect(r.bindings.size() == 2u && keys != nullptr && scratch != nullptr,
           (prefix + ": bindings (0,0) and (0,4)").c_str());
    expect(keys != nullptr && keys->descriptorType == kDescStorageBuffer && keys->count == 1u,
           (prefix + ": keys is one storage buffer").c_str());
    expect(scratch != nullptr && scratch->descriptorType == kDescStorageBuffer,
           (prefix + ": scratch is a storage buffer").c_str());
}

void runHistogramTwin(Context& ctx) {
    const std::vector<u32> glslWords = loadWords(FUSE_RP_GLSL_HISTOGRAM_SPV);
    const std::vector<u32> slangWords = loadWords(FUSE_RP_SLANG_HISTOGRAM_SPV);
    expect(!glslWords.empty() && !slangWords.empty(), "histogram SPIR-V (GLSL + Slang) loaded");
    if (glslWords.empty() || slangWords.empty()) {
        return;
    }
    ReflectedKernel glsl;
    ReflectedKernel slang;
    expect(glsl.build(*ctx.device, glslWords, "rp_twin.histogram.glsl"), "GLSL histogram pipeline");
    expect(slang.build(*ctx.device, slangWords, "rp_twin.histogram.slang"), "Slang histogram pipeline");
    if (!glsl.pipeline || !glsl.pipeline->isValid() || !slang.pipeline || !slang.pipeline->isValid()) {
        return;
    }
    checkHistogramReflection(glsl.reflection, "GLSL histogram");
    checkHistogramReflection(slang.reflection, "Slang histogram");
    expect(sameLayout(glsl.reflection, slang.reflection), "GLSL and Slang histogram reflect the same layout");

    constexpr u32 kCount = 10000;
    constexpr u32 kTile = 4096;
    constexpr u32 kBins = 256;
    const u32 numBlocks = (kCount + kTile - 1u) / kTile;
    HostBuffer keys;
    HostBuffer scratchGlsl;
    HostBuffer scratchSlang;
    expect(keys.create(ctx, kCount * 4u) && scratchGlsl.create(ctx, kBins * numBlocks * 4u) &&
               scratchSlang.create(ctx, kBins * numBlocks * 4u),
           "histogram buffers");
    u32 seed = 12345u;
    auto* keyData = static_cast<u32*>(keys.mapped);
    for (u32 i = 0; i < kCount; ++i) {
        keyData[i] = lcg(seed);
    }
    std::memset(scratchGlsl.mapped, 0xCD, scratchGlsl.size);
    std::memset(scratchSlang.mapped, 0xAB, scratchSlang.size);

    for (const u32 shift : {0u, 8u, 24u}) {
        const u32 push[6] = {kCount, shift, numBlocks, 0u, 0u, 0u};
        expect(glsl.run(ctx, {{0, &keys}, {4, &scratchGlsl}}, push, numBlocks), "GLSL histogram dispatch");
        expect(slang.run(ctx, {{0, &keys}, {4, &scratchSlang}}, push, numBlocks), "Slang histogram dispatch");
        const bool identical = std::memcmp(scratchGlsl.mapped, scratchSlang.mapped, scratchGlsl.size) == 0;
        expect(identical, "Slang histogram is bit-identical to the GLSL twin");

        std::vector<u32> reference(kBins * numBlocks, 0u);
        for (u32 i = 0; i < kCount; ++i) {
            ++reference[((keyData[i] >> shift) & 0xFFu) * numBlocks + i / kTile];
        }
        expect(std::memcmp(reference.data(), scratchSlang.mapped, scratchSlang.size) == 0,
               "Slang histogram equals the CPU reference");
        std::printf("histogram shift %2u: %u tiles x 256 bins, GLSL == Slang: %s\n", shift, numBlocks,
                    identical ? "bit-identical" : "DIFFERENT");
    }
}

void runFloatTwin(Context& ctx) {
    const std::vector<u32> glslWords = loadWords(FUSE_RP_GLSL_FLOAT_SPV);
    const std::vector<u32> slangWords = loadWords(FUSE_RP_SLANG_FLOAT_SPV);
    expect(!glslWords.empty() && !slangWords.empty(), "float SPIR-V (GLSL + Slang) loaded");
    if (glslWords.empty() || slangWords.empty()) {
        return;
    }
    ReflectedKernel glsl;
    ReflectedKernel slang;
    expect(glsl.build(*ctx.device, glslWords, "rp_twin.float.glsl"), "GLSL float pipeline");
    expect(slang.build(*ctx.device, slangWords, "rp_twin.float.slang"), "Slang float pipeline");
    if (!glsl.pipeline || !glsl.pipeline->isValid() || !slang.pipeline || !slang.pipeline->isValid()) {
        return;
    }
    expect(sameLayout(glsl.reflection, slang.reflection), "GLSL and Slang float kernel reflect the same layout");
    expect(slang.reflection.pushConstantBytes == 16u && slang.reflection.localSize[0] == 64u &&
               slang.reflection.bindings.size() == 2u,
           "float kernel reflection: 16-byte push, 64 threads, 2 bindings");

    constexpr u32 kCount = 1000;
    HostBuffer inputs;
    HostBuffer outGlsl;
    HostBuffer outSlang;
    expect(inputs.create(ctx, kCount * 16u) && outGlsl.create(ctx, kCount * 16u) && outSlang.create(ctx, kCount * 16u),
           "float buffers");
    auto* in = static_cast<float*>(inputs.mapped);
    u32 seed = 777u;
    for (u32 i = 0; i < kCount * 4u; ++i) {
        in[i] = (static_cast<float>(lcg(seed) >> 8) / 16777216.0f - 0.5f) * 20.0f;
    }
    struct Push {
        u32 count;
        float scale;
        float bias;
        u32 pad;
    } push{kCount, 1.37f, -0.25f, 0u};
    expect(glsl.run(ctx, {{0, &inputs}, {1, &outGlsl}}, &push, (kCount + 63u) / 64u), "GLSL float dispatch");
    expect(slang.run(ctx, {{0, &inputs}, {1, &outSlang}}, &push, (kCount + 63u) / 64u), "Slang float dispatch");
    const bool identical = std::memcmp(outGlsl.mapped, outSlang.mapped, outGlsl.size) == 0;
    expect(identical, "Slang float kernel is bit-identical to the GLSL twin");

    // Sanity: it computed the formula (not two identical garbage buffers).
    const auto* out = static_cast<const float*>(outSlang.mapped);
    u32 bad = 0;
    for (u32 i = 0; i < kCount; ++i) {
        const float* v = in + i * 4u;
        float x = v[0] + 1e-3f, y = v[1] + 1e-3f, z = v[2] + 1e-3f;
        const float len = std::sqrt(x * x + y * y + z * z);
        x /= len;
        y /= len;
        z /= len;
        const float s = std::sqrt(std::fabs(v[3])) * push.scale + push.bias;
        if (std::fabs(out[i * 4u] - x * s) > 1e-3f || std::fabs(out[i * 4u + 1u] - y * s) > 1e-3f ||
            std::fabs(out[i * 4u + 2u] - z * s) > 1e-3f) {
            ++bad;
        }
    }
    expect(bad == 0u, "float kernel output matches the CPU formula within 1e-3");
    std::printf("float twin: %u vec4, GLSL == Slang: %s\n", kCount, identical ? "bit-identical" : "DIFFERENT");
}

void checkReflectionJson() {
    std::ifstream in(FUSE_RP_SLANG_HISTOGRAM_JSON, std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(!json.empty(), "slangc reflection JSON written next to the SPIR-V");
    expect(json.find("\"keysIn\"") != std::string::npos && json.find("\"scratch\"") != std::string::npos &&
               json.find("pushConstantBuffer") != std::string::npos,
           "reflection JSON names the bindings and the push-constant buffer");
}

void checkRuntimeCompile() {
    // Runtime Slang compile of the same source reflects the same layout as the build-time output.
    ShaderDesc desc{};
    desc.sourcePath = FUSE_RP_SLANG_HISTOGRAM_SRC;
    desc.stage = ShaderStage::Compute;
    const CompiledShader runtime = ShaderCompiler::compileWithSlang(desc);
    expect(runtime.valid, runtime.valid ? "runtime slangc compile" : runtime.message.c_str());
    if (!runtime.valid) {
        return;
    }
    const std::vector<u32> buildTime = loadWords(FUSE_RP_SLANG_HISTOGRAM_SPV);
    const ShaderReflection a = reflectSpirv(runtime.spirv.data(), runtime.spirv.size());
    const ShaderReflection b = reflectSpirv(buildTime.data(), buildTime.size());
    expect(sameLayout(a, b), "runtime and build-time Slang SPIR-V reflect the same layout");
    std::printf("runtime compile: %zu words (build time %zu), identical: %s, deps: %zu\n", runtime.spirv.size(),
                buildTime.size(), runtime.spirv == buildTime ? "yes" : "no", runtime.dependencies.size());
    bool importsCommon = false;
    for (const std::string& dep : runtime.dependencies) {
        importsCommon = importsCommon || dep.find("rp_twin_common.slang") != std::string::npos;
    }
    expect(importsCommon, "runtime compile reports the imported module as a dependency");

    // Permutation key: defines are order independent; tier and define values change it.
    const char* definesAB[] = {"FUSE_A=1", "FUSE_B=2"};
    const char* definesBA[] = {"FUSE_B=2", "FUSE_A=1"};
    ShaderDesc p1 = desc;
    p1.defines = definesAB;
    p1.defineCount = 2;
    ShaderDesc p2 = desc;
    p2.defines = definesBA;
    p2.defineCount = 2;
    ShaderDesc p3 = p1;
    p3.tier = 2;
    expect(shaderPermutationKey(p1) == shaderPermutationKey(p2), "permutation key ignores define order");
    expect(shaderPermutationKey(p1) != shaderPermutationKey(desc), "defines change the permutation key");
    expect(shaderPermutationKey(p3) != shaderPermutationKey(p1), "tier changes the permutation key");

    // Background compile: four permutations of the float kernel on the JobScheduler.
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    if (scheduler.isInitialized() && scheduler.workerCount() < 2u) {
        scheduler.setWorkerCount(2u);
    }
    const char* d0[] = {"FUSE_TWIN_VARIANT=0"};
    const char* d1[] = {"FUSE_TWIN_VARIANT=1"};
    ShaderDesc batch[4]{};
    for (u32 i = 0; i < 4u; ++i) {
        batch[i].sourcePath = FUSE_RP_SLANG_FLOAT_SRC;
        batch[i].stage = ShaderStage::Compute;
        batch[i].defines = (i & 1u) != 0u ? d1 : d0;
        batch[i].defineCount = 1;
        batch[i].tier = (i & 2u) != 0u ? 2 : -1;
    }
    const auto start = std::chrono::steady_clock::now();
    const std::vector<CompiledShader> results = ShaderCompiler::compileBatch(batch, 4u, &scheduler);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    bool allValid = results.size() == 4u;
    for (const CompiledShader& r : results) {
        allValid = allValid && r.valid;
    }
    expect(allValid, "background compile of 4 Slang permutations succeeded");
    if (allValid) {
        expect(results[0].permutationKey != results[1].permutationKey &&
                   results[0].permutationKey != results[2].permutationKey &&
                   results[2].permutationKey != results[3].permutationKey,
               "each permutation has its own key");
        expect(results[0].spirvPath != results[3].spirvPath, "permutations land in distinct cache files");
        expect(results[2].tier == 2, "tier recorded on the compiled permutation");
    }
    std::printf("compileBatch: 4 permutations on %u workers in %.1f ms\n", scheduler.workerCount(), ms);
}

#endif // FUSE_RP_SLANG_TWIN_BUILT

int modeTwin() {
#if !defined(FUSE_RP_SLANG_TWIN_BUILT)
    std::printf("SKIP: Slang twin shaders not built (FUSE_SLANG=OFF or no slangc / glslangValidator)\n");
    return kSkipCode;
#else
    Context ctx;
    const int setup = rp_wp05::setupContext(ctx, "fuse_rp_slang_twin");
    if (setup != 0) {
        return setup;
    }
    runHistogramTwin(ctx);
    runFloatTwin(ctx);
    checkReflectionJson();
    checkRuntimeCompile();
    ctx.device->waitIdle();
    expect(rp_wp05::validationMessageCount() == 0u, "zero validation messages");
    std::printf("validation messages: %u\n", rp_wp05::validationMessageCount());
    return finish("twin");
#endif
}

int modeSpirvVal() {
#if !defined(FUSE_RP_SLANG_TWIN_BUILT) || !defined(FUSE_RP_SPIRV_VAL)
    std::printf("SKIP: Slang twin shaders or spirv-val unavailable\n");
    return kSkipCode;
#else
    std::vector<std::string> modules = {FUSE_RP_SLANG_HISTOGRAM_SPV, FUSE_RP_SLANG_FLOAT_SPV};
    ShaderDesc desc{};
    desc.sourcePath = FUSE_RP_SLANG_FLOAT_SRC;
    desc.stage = ShaderStage::Compute;
    desc.tier = 1;
    const CompiledShader runtime = ShaderCompiler::compileWithSlang(desc);
    expect(runtime.valid, "runtime Slang compile for spirv-val");
    if (runtime.valid) {
        modules.push_back(runtime.spirvPath);
    }
    for (const std::string& path : modules) {
        const std::string command = std::string("\"") + FUSE_RP_SPIRV_VAL + "\" --target-env vulkan1.3 \"" + path + "\"";
        const int rc = std::system(command.c_str());
        std::printf("spirv-val %s: %s\n", path.c_str(), rc == 0 ? "ok" : "FAILED");
        expect(rc == 0, "spirv-val accepts the Slang SPIR-V");
    }
    return finish("spirv-val");
#endif
}

// ---- hot reload ------------------------------------------------------------------------------------

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    // Push the mtime forward so coarse filesystem timestamps still register the edit.
    std::error_code ec;
    static int bump = 0;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++bump),
                                     ec);
    return static_cast<bool>(out) || !out.bad();
}

/// Dispatches a 1-thread kernel writing one uint to binding 0 and returns it (0xFFFFFFFF on error).
u32 runValueKernel(Context& ctx, const CompiledShader& compiled) {
    if (!compiled.valid) {
        return 0xFFFFFFFFu;
    }
    ReflectedKernel kernel;
    if (!kernel.build(*ctx.device, compiled.spirv, "rp_slang_hot_reload")) {
        return 0xFFFFFFFFu;
    }
    HostBuffer out;
    if (!out.create(ctx, 16u)) {
        return 0xFFFFFFFFu;
    }
    std::memset(out.mapped, 0, 16u);
    if (!kernel.run(ctx, {{0, &out}}, nullptr, 1u)) {
        return 0xFFFFFFFFu;
    }
    return static_cast<const u32*>(out.mapped)[0];
}

bool hasDependency(const CompiledShader& compiled, const char* fileName) {
    for (const std::string& dep : compiled.dependencies) {
        if (std::filesystem::path(dep).filename() == fileName) {
            return true;
        }
    }
    return false;
}

int modeHotReload() {
    if (ShaderCompiler::defaultSlangcPath() == nullptr) {
        std::printf("SKIP: no slangc (FUSE_SLANG=OFF or not resolved)\n");
        return kSkipCode;
    }
    Context ctx;
    const int setup = rp_wp05::setupContext(ctx, "fuse_rp_slang_hot_reload");
    if (setup != 0) {
        return setup;
    }

    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("fuse_rp_slang_hot_reload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path module = dir / "hr_common.slang";
    const std::filesystem::path include = dir / "hr_offset.slangh";
    const std::filesystem::path mainPath = dir / "hr_main.slang";
    writeText(module, "module hr_common;\npublic static const uint kBase = 7;\n");
    writeText(include, "#define HR_OFFSET 100u\n");
    writeText(mainPath, "import hr_common;\n#include \"hr_offset.slangh\"\n"
                        "[[vk::binding(0, 0)]] RWStructuredBuffer<uint> result;\n"
                        "[shader(\"compute\")] [numthreads(1, 1, 1)]\n"
                        "void main() { result[0] = kBase + HR_OFFSET; }\n");

    ShaderCompiler compiler;
    expect(compiler.enableSlangRuntimeCompile(), "Slang runtime compile enabled");
    const std::string mainString = mainPath.string();
    ShaderDesc desc{};
    desc.sourcePath = mainString.c_str();
    desc.stage = ShaderStage::Compute;
    expect(compiler.watch(desc), "compiler watches the Slang kernel");
    const CompiledShader* first = compiler.lastCompiled(mainString.c_str());
    expect(first != nullptr && first->valid, "initial Slang compile");
    if (first == nullptr || !first->valid) {
        if (first != nullptr) {
            std::fprintf(stderr, "%s\n", first->message.c_str());
        }
        return finish("hot-reload");
    }
    expect(hasDependency(*first, "hr_common.slang"), "depfile lists the imported module");
    expect(hasDependency(*first, "hr_offset.slangh"), "depfile lists the #included file");
    const u64 hash0 = first->spirvHash;
    expect(runValueKernel(ctx, *first) == 107u, "initial kernel writes 7 + 100");
    expect(compiler.pollHotReload() == 0u, "no edit, no recompile");

    writeText(module, "module hr_common;\npublic static const uint kBase = 11;\n");
    expect(compiler.pollHotReload() == 1u, "editing the imported Slang module recompiles the kernel once");
    const CompiledShader* second = compiler.lastCompiled(mainString.c_str());
    expect(second != nullptr && second->valid && second->spirvHash != hash0, "module edit produced new SPIR-V");
    expect(second != nullptr && runValueKernel(ctx, *second) == 111u, "kernel sees the edited module (11 + 100)");

    writeText(include, "#define HR_OFFSET 200u\n");
    expect(compiler.pollHotReload() == 1u, "editing the #included file recompiles the kernel once");
    const CompiledShader* third = compiler.lastCompiled(mainString.c_str());
    expect(third != nullptr && third->valid && runValueKernel(ctx, *third) == 211u,
           "kernel sees the edited include (11 + 200)");

    writeText(module, "module hr_common;\npublic static const uint kBase = ;\n");
    expect(compiler.pollHotReload() == 0u, "broken module edit fails to compile");
    writeText(module, "module hr_common;\npublic static const uint kBase = 5;\n");
    expect(compiler.pollHotReload() == 1u, "fixing the module after a failed build recompiles again");
    const CompiledShader* fourth = compiler.lastCompiled(mainString.c_str());
    expect(fourth != nullptr && fourth->valid && runValueKernel(ctx, *fourth) == 205u,
           "kernel recovered after the broken edit (5 + 200)");

    // GLSL #include through glslangValidator's depfile.
    if (ShaderCompiler::defaultValidatorPath() != nullptr) {
        const std::filesystem::path glslInclude = dir / "hr_value.glsl";
        const std::filesystem::path glslMain = dir / "hr_main.comp";
        writeText(glslInclude, "const uint kValue = 3u;\n");
        writeText(glslMain, "#version 450\n#extension GL_GOOGLE_include_directive : require\n"
                            "#include \"hr_value.glsl\"\nlayout(local_size_x = 1) in;\n"
                            "layout(std430, set = 0, binding = 0) buffer Result { uint result[]; };\n"
                            "void main() { result[0] = kValue; }\n");
        ShaderCompiler glslCompiler;
        expect(glslCompiler.enableRuntimeCompile(), "GLSL runtime compile enabled");
        const std::string glslString = glslMain.string();
        ShaderDesc glslDesc{};
        glslDesc.sourcePath = glslString.c_str();
        glslDesc.stage = ShaderStage::Compute;
        expect(glslCompiler.watch(glslDesc), "compiler watches the GLSL kernel");
        const CompiledShader* g0 = glslCompiler.lastCompiled(glslString.c_str());
        expect(g0 != nullptr && g0->valid && hasDependency(*g0, "hr_value.glsl"), "GLSL depfile lists the include");
        expect(g0 != nullptr && runValueKernel(ctx, *g0) == 3u, "GLSL kernel writes 3");
        writeText(glslInclude, "const uint kValue = 9u;\n");
        expect(glslCompiler.pollHotReload() == 1u, "editing the GLSL include recompiles once");
        const CompiledShader* g1 = glslCompiler.lastCompiled(glslString.c_str());
        expect(g1 != nullptr && g1->valid && runValueKernel(ctx, *g1) == 9u, "GLSL kernel sees the edited include");
    }

    ctx.device->waitIdle();
    std::filesystem::remove_all(dir, ec);
    expect(rp_wp05::validationMessageCount() == 0u, "zero validation messages");
    return finish("hot-reload");
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "twin";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        }
    }
    fuse::core::initialize();
    int rc = EXIT_FAILURE;
    if (mode == "twin") {
        rc = modeTwin();
    } else if (mode == "spirv-val") {
        rc = modeSpirvVal();
    } else if (mode == "hot-reload") {
        rc = modeHotReload();
    } else {
        std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
    }
    fuse::core::shutdown();
    return rc;
}

#endif // FUSE_VULKAN_BACKEND
