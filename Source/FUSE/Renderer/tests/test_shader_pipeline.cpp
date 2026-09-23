#include <fuse/core/temp_path.hpp>
#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/shader/shader_compiler.hpp>
#include <fuse/renderer/shader/shader_io.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/compute_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/types.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#ifndef FUSE_SHADER_FIXTURE_DIR
#define FUSE_SHADER_FIXTURE_DIR "Source/FUSE/Renderer/shaders/fixtures"
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string fixturePath(const char* name) {
    return std::string(FUSE_SHADER_FIXTURE_DIR) + "/" + name;
}

bool writeFile(const std::filesystem::path& path, const char* contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << contents;
    out.flush();
    return static_cast<bool>(out);
}

std::filesystem::path uniqueTempShaderPath() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(::getpid());
#endif
    static int seq = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("fuse_shader_compiler_" + std::to_string(pid) + "_" + std::to_string(stamp) + "_" +
            std::to_string(++seq) + ".glsl");
}

void testSpirvIo() {
    const std::string vertPath = fixturePath("minimal.vert.spv");
    std::string error;
    const std::vector<fuse::u32> words = fuse::renderer::loadSpirvFile(vertPath.c_str(), &error);
    expectTrue(!words.empty(), "fixture SPIR-V loads");
    expectTrue(fuse::renderer::isValidSpirvHeader(words.data(), static_cast<fuse::u32>(words.size())),
               "fixture SPIR-V header valid");
    expectTrue(error.empty(), "fixture SPIR-V load has no error");
}

void testOfflineCompiler() {
    const std::string sourcePath = fixturePath("minimal.vert");
    fuse::renderer::ShaderDesc desc{};
    desc.sourcePath = sourcePath.c_str();
    desc.stage = fuse::renderer::ShaderStage::Vertex;

    const fuse::renderer::CompiledShader compiled = fuse::renderer::ShaderCompiler::compileOffline(desc);
    expectTrue(compiled.valid, "offline compiler loads sibling .spv fixture");
    expectTrue(fuse::renderer::isValidSpirvHeader(compiled.spirv.data(),
                                                  static_cast<fuse::u32>(compiled.spirv.size())),
               "compiled SPIR-V header valid");
    expectTrue(!compiled.spirv.empty(), "compiled SPIR-V non-empty");
    expectTrue(!compiled.valid || compiled.spirvHash != 0, "valid compiled shader has non-zero spirvHash");
    expectTrue(compiled.entryPoint == "main", "compiled shader entryPoint is main");
    expectTrue(compiled.spirvHash == fuse::renderer::hashSpirvWords(
                   compiled.spirv.data(), static_cast<fuse::u32>(compiled.spirv.size())),
               "spirvHash matches hashSpirvWords of loaded words");
}

void testOfflineCompilerDefineVariants() {
    const std::string sourcePath = fixturePath("minimal.vert");

    const char* definesFoo1[] = {"FOO=1"};
    fuse::renderer::ShaderDesc descFoo1{};
    descFoo1.sourcePath = sourcePath.c_str();
    descFoo1.stage = fuse::renderer::ShaderStage::Vertex;
    descFoo1.defines = definesFoo1;
    descFoo1.defineCount = 1u;

    const char* definesFoo2[] = {"FOO=2"};
    fuse::renderer::ShaderDesc descFoo2{};
    descFoo2.sourcePath = sourcePath.c_str();
    descFoo2.stage = fuse::renderer::ShaderStage::Vertex;
    descFoo2.defines = definesFoo2;
    descFoo2.defineCount = 1u;

    const fuse::renderer::CompiledShader compiledFoo1 =
        fuse::renderer::ShaderCompiler::compileOffline(descFoo1);
    const fuse::renderer::CompiledShader compiledFoo2 =
        fuse::renderer::ShaderCompiler::compileOffline(descFoo2);

    expectTrue(compiledFoo1.valid && compiledFoo2.valid, "offline define variants both load fixture SPIR-V");
    expectTrue(compiledFoo1.spirv == compiledFoo2.spirv, "offline define variants load the same SPIR-V words");
    expectTrue(compiledFoo1.defineCount == 1u && compiledFoo2.defineCount == 1u,
               "offline define variants report defineCount 1");
    expectTrue(compiledFoo1.defines.size() == 1u && compiledFoo1.defines[0] == "FOO=1",
               "FOO=1 variant owns define string");
    expectTrue(compiledFoo2.defines.size() == 1u && compiledFoo2.defines[0] == "FOO=2",
               "FOO=2 variant owns define string");
    expectTrue(compiledFoo1.spirvHash != compiledFoo2.spirvHash,
               "define variants produce different spirvHash keys");

    const fuse::u64 wordsHash = fuse::renderer::hashSpirvWords(
        compiledFoo1.spirv.data(), static_cast<fuse::u32>(compiledFoo1.spirv.size()));
    expectTrue(compiledFoo1.spirvHash != wordsHash && compiledFoo2.spirvHash != wordsHash,
               "define mix changes hash from raw SPIR-V words");
}

void testOfflineCompilerIncludePathVariants() {
    const std::string sourcePath = fixturePath("minimal.vert");

    const char* includeA[] = {"a"};
    fuse::renderer::ShaderDesc descA{};
    descA.sourcePath = sourcePath.c_str();
    descA.stage = fuse::renderer::ShaderStage::Vertex;
    descA.includePaths = includeA;
    descA.includePathCount = 1u;

    const char* includeB[] = {"b"};
    fuse::renderer::ShaderDesc descB{};
    descB.sourcePath = sourcePath.c_str();
    descB.stage = fuse::renderer::ShaderStage::Vertex;
    descB.includePaths = includeB;
    descB.includePathCount = 1u;

    const fuse::renderer::CompiledShader compiledA =
        fuse::renderer::ShaderCompiler::compileOffline(descA);
    const fuse::renderer::CompiledShader compiledB =
        fuse::renderer::ShaderCompiler::compileOffline(descB);

    expectTrue(compiledA.valid && compiledB.valid, "offline include-path variants both load fixture SPIR-V");
    expectTrue(compiledA.spirv == compiledB.spirv, "offline include-path variants load the same SPIR-V words");
    expectTrue(compiledA.includePathCount == 1u && compiledB.includePathCount == 1u,
               "offline include-path variants report includePathCount 1");
    expectTrue(compiledA.includePaths.size() == 1u && compiledA.includePaths[0] == "a",
               "include path a variant owns include path string");
    expectTrue(compiledB.includePaths.size() == 1u && compiledB.includePaths[0] == "b",
               "include path b variant owns include path string");
    expectTrue(compiledA.spirvHash != compiledB.spirvHash,
               "include-path variants produce different spirvHash keys");

    const fuse::u64 wordsHash = fuse::renderer::hashSpirvWords(
        compiledA.spirv.data(), static_cast<fuse::u32>(compiledA.spirv.size()));
    expectTrue(compiledA.spirvHash != wordsHash && compiledB.spirvHash != wordsHash,
               "include-path mix changes hash from raw SPIR-V words");
}

void testCookedFuseshaderLoader() {
    const std::string spirvPath = fixturePath("minimal.vert.spv");
    std::ifstream spirvIn(spirvPath, std::ios::binary);
    expectTrue(spirvIn.good(), "fixture spirv readable for cooked loader test");

    std::vector<char> spirvBytes((std::istreambuf_iterator<char>(spirvIn)),
                                 std::istreambuf_iterator<char>());
    const std::string cookedPath = fuse::test::tempPath("fuse_cooked_loader_test.fuseshader");
    std::ofstream cookedOut(cookedPath, std::ios::binary | std::ios::trunc);
    cookedOut << "FUSESHADER_SPIV\nstage=vertex\nversion=450\nwords="
              << (spirvBytes.size() / 4u) << "\nDATA\n";
    cookedOut.write(spirvBytes.data(), static_cast<std::streamsize>(spirvBytes.size()));
    cookedOut.close();

    std::string error;
    const std::vector<fuse::u32> words =
        fuse::renderer::loadCookedFuseshaderSpirv(cookedPath.c_str(), &error);
    expectTrue(!words.empty(), "cooked fuseshader loader extracts SPIR-V payload");
    expectTrue(fuse::renderer::isValidSpirvHeader(words.data(), static_cast<fuse::u32>(words.size())),
               "cooked fuseshader SPIR-V header valid");
    expectTrue(error.empty(), "cooked fuseshader loader has no error");
}

void testCreateFromCompiledShader() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for compiled shader module tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

    fuse::renderer::CompiledShader invalid{};
    auto invalidModule = fuse::renderer::ShaderModule::create(*device, invalid);
    expectTrue(invalidModule != nullptr, "invalid compiled shader still allocates module");
    expectTrue(!invalidModule->isValid(), "invalid compiled shader yields invalid module");

    fuse::renderer::CompiledShader emptySpirv{};
    emptySpirv.valid = true;
    emptySpirv.stage = fuse::renderer::ShaderStage::Fragment;
    auto emptyModule = fuse::renderer::ShaderModule::create(*device, emptySpirv);
    expectTrue(emptyModule != nullptr, "empty SPIR-V compiled shader still allocates module");
    expectTrue(!emptyModule->isValid(), "empty SPIR-V compiled shader yields invalid module");
    expectTrue(emptyModule->stage() == fuse::renderer::ShaderStage::Fragment,
               "empty SPIR-V compiled shader preserves stage");

    const std::string sourcePath = fixturePath("minimal.vert");
    fuse::renderer::ShaderDesc desc{};
    desc.sourcePath = sourcePath.c_str();
    desc.stage = fuse::renderer::ShaderStage::Vertex;
    const fuse::renderer::CompiledShader compiled = fuse::renderer::ShaderCompiler::compileOffline(desc);
    expectTrue(compiled.valid, "offline compile for module create succeeds");
    expectTrue(!compiled.spirv.empty(), "offline compile SPIR-V non-empty for module create");

    auto shaderModule = fuse::renderer::ShaderModule::create(*device, compiled);
    expectTrue(shaderModule != nullptr, "compiled shader module allocated");
    expectTrue(shaderModule->stage() == fuse::renderer::ShaderStage::Vertex,
               "compiled shader module stage matches");
    expectTrue(shaderModule->info().entryPoint == compiled.entryPoint,
               "compiled shader module stores entry point");
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(shaderModule->isValid(), "compiled shader module valid with Vulkan device");
        expectTrue(shaderModule->nativeHandle() != nullptr, "compiled shader module has native handle");
    } else {
        expectTrue(!shaderModule->isValid(), "compiled shader module invalid without ICD");
    }
#else
    expectTrue(shaderModule->isValid(), "compiled shader module valid in stub backend");
    expectTrue(shaderModule->nativeHandle() == nullptr, "stub compiled shader module has no native handle");
#endif
}

void testShaderModuleAndPipelineLayout() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for shader tests");

    const std::string vertPath = fixturePath("minimal.vert.spv");
    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

    auto shaderModule = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Vertex, vertPath.c_str());
    expectTrue(shaderModule != nullptr, "shader module allocated");
    if (shaderModule->isValid()) {
        std::string hashError;
        const std::vector<fuse::u32> words =
            fuse::renderer::loadSpirvFile(vertPath.c_str(), &hashError);
        expectTrue(shaderModule->info().spirvHash != 0, "valid shader module has non-zero spirvHash");
        expectTrue(shaderModule->info().spirvHash ==
                       fuse::renderer::hashSpirvWords(words.data(),
                                                      static_cast<fuse::u32>(words.size())),
                   "shader module spirvHash matches hashSpirvWords of fixture");
    }
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(shaderModule->isValid(), "shader module valid with Vulkan device");
        expectTrue(shaderModule->nativeHandle() != nullptr, "shader module has native handle");
    } else {
        expectTrue(!shaderModule->isValid(), "shader module invalid without ICD");
    }
#else
    expectTrue(shaderModule->isValid(), "shader module valid in stub backend");
    expectTrue(shaderModule->nativeHandle() == nullptr, "stub backend has no native handle");
#endif

    fuse::renderer::PipelineLayoutDesc layoutDesc{};
    layoutDesc.debugName = "test_layout";
    layoutDesc.pushConstants.push_back({0u, 16u, 0x00000010u});

    auto pipelineLayout = fuse::renderer::PipelineLayout::create(*device, layoutDesc);
    expectTrue(pipelineLayout != nullptr, "pipeline layout allocated");
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(pipelineLayout->isValid(), "pipeline layout valid with Vulkan device");
        expectTrue(pipelineLayout->nativeHandle() != nullptr, "pipeline layout has native handle");
        expectTrue(!pipelineLayout->info().hasBindlessSet,
                   "push-constant-only layout has no bindless set");
        expectTrue(pipelineLayout->info().descriptorSetCount == 0u,
                   "push-constant-only layout reports zero descriptor sets");
        expectTrue(pipelineLayout->info().pushConstantRangeCount == 1u,
                   "push-constant-only layout reports one push-constant range");
    } else {
        expectTrue(!pipelineLayout->isValid(), "pipeline layout invalid without ICD");
        expectTrue(!pipelineLayout->info().hasBindlessSet,
                   "failed push-constant-only layout has no bindless set");
    }
#else
    expectTrue(pipelineLayout->isValid(), "pipeline layout valid in stub backend");
    expectTrue(!pipelineLayout->info().hasBindlessSet,
               "stub push-constant-only layout has no bindless set");
    expectTrue(pipelineLayout->info().descriptorSetCount == 0u,
               "stub push-constant-only layout reports zero descriptor sets");
#endif
}

void testHotReloadPoller() {
    const std::filesystem::path glslPathA = uniqueTempShaderPath();
    const std::filesystem::path glslPathB = uniqueTempShaderPath();
    const std::string glslUtf8A = glslPathA.string();
    const std::string glslUtf8B = glslPathB.string();
    const std::filesystem::path spvPathA(glslUtf8A + ".spv");
    const std::filesystem::path spvPathB(glslUtf8B + ".spv");

    expectTrue(writeFile(glslPathA, "void main() {}\n"), "hot-reload temp glsl A created");
    expectTrue(writeFile(glslPathB, "void main() {}\n"), "hot-reload temp glsl B created");

    const std::string fixtureSpv = fixturePath("minimal.vert.spv");
    std::ifstream spirvIn(fixtureSpv, std::ios::binary);
    expectTrue(spirvIn.good(), "fixture spirv readable for hot-reload test");
    std::vector<char> spirvBytes((std::istreambuf_iterator<char>(spirvIn)),
                                 std::istreambuf_iterator<char>());
    expectTrue(!spirvBytes.empty(), "fixture spirv non-empty for hot-reload test");

    std::ofstream spirvOutA(spvPathA, std::ios::binary | std::ios::trunc);
    spirvOutA.write(spirvBytes.data(), static_cast<std::streamsize>(spirvBytes.size()));
    spirvOutA.flush();
    expectTrue(static_cast<bool>(spirvOutA), "sibling spirv written for hot-reload test A");
    spirvOutA.close();

    std::ofstream spirvOutB(spvPathB, std::ios::binary | std::ios::trunc);
    spirvOutB.write(spirvBytes.data(), static_cast<std::streamsize>(spirvBytes.size()));
    spirvOutB.flush();
    expectTrue(static_cast<bool>(spirvOutB), "sibling spirv written for hot-reload test B");
    spirvOutB.close();

    fuse::renderer::ShaderDesc descA{};
    descA.sourcePath = glslUtf8A.c_str();
    descA.stage = fuse::renderer::ShaderStage::Vertex;

    fuse::renderer::ShaderDesc descB{};
    descB.sourcePath = glslUtf8B.c_str();
    descB.stage = fuse::renderer::ShaderStage::Vertex;

    fuse::renderer::ShaderCompiler compiler;
    expectTrue(compiler.watch(descA), "watch records shader path A");
    expectTrue(compiler.watch(descB), "watch records shader path B");
    expectTrue(compiler.watchedCount() == 2u, "hot-reload watched count is 2");

    const fuse::renderer::CompiledShader* compiledA = compiler.lastCompiled(glslUtf8A.c_str());
    expectTrue(compiledA != nullptr && compiledA->valid, "watch compiles offline SPIR-V A");
    const fuse::renderer::CompiledShader* compiledB = compiler.lastCompiled(glslUtf8B.c_str());
    expectTrue(compiledB != nullptr && compiledB->valid, "watch compiles offline SPIR-V B");
    expectTrue(compiler.pollHotReload() == 0u, "unchanged files report no hot reload");

    expectTrue(writeFile(glslPathA, "void main() { /* hot reload */ }\n"), "temp shader A rewritten");
    expectTrue(compiler.pollHotReload() == 1u, "rewrite of one shader recompiles only that path");

    compiledA = compiler.lastCompiled(glslUtf8A.c_str());
    expectTrue(compiledA != nullptr && compiledA->valid, "rewritten shader lastCompiled still valid");
    compiledB = compiler.lastCompiled(glslUtf8B.c_str());
    expectTrue(compiledB != nullptr && compiledB->valid, "untouched shader lastCompiled still valid");

    std::error_code ec;
    std::filesystem::remove(glslPathA, ec);
    std::filesystem::remove(spvPathA, ec);
    std::filesystem::remove(glslPathB, ec);
    std::filesystem::remove(spvPathB, ec);
}

void testWatchCopiesDefineStrings() {
    const std::filesystem::path glslPath = uniqueTempShaderPath();
    const std::string glslUtf8 = glslPath.string();
    const std::filesystem::path spvPath(glslUtf8 + ".spv");

    expectTrue(writeFile(glslPath, "void main() {}\n"), "define-watch temp glsl created");

    const std::string fixtureSpv = fixturePath("minimal.vert.spv");
    std::ifstream spirvIn(fixtureSpv, std::ios::binary);
    expectTrue(spirvIn.good(), "fixture spirv readable for define-watch test");
    std::vector<char> spirvBytes((std::istreambuf_iterator<char>(spirvIn)),
                                 std::istreambuf_iterator<char>());
    expectTrue(!spirvBytes.empty(), "fixture spirv non-empty for define-watch test");

    std::ofstream spirvOut(spvPath, std::ios::binary | std::ios::trunc);
    spirvOut.write(spirvBytes.data(), static_cast<std::streamsize>(spirvBytes.size()));
    spirvOut.flush();
    expectTrue(static_cast<bool>(spirvOut), "sibling spirv written for define-watch test");
    spirvOut.close();

    fuse::renderer::ShaderCompiler compiler;
    {
        const char* stackDefines[] = {"FOO=1"};
        fuse::renderer::ShaderDesc desc{};
        desc.sourcePath = glslUtf8.c_str();
        desc.stage = fuse::renderer::ShaderStage::Vertex;
        desc.defines = stackDefines;
        desc.defineCount = 1u;
        expectTrue(compiler.watch(desc), "watch copies stack define strings");
    }

    const fuse::renderer::CompiledShader* compiled = compiler.lastCompiled(glslUtf8.c_str());
    expectTrue(compiled != nullptr && compiled->valid, "watch compile with defines is valid");
    expectTrue(compiled->defineCount == 1u, "watch lastCompiled defineCount is 1");
    expectTrue(compiled->defines.size() == 1u && compiled->defines[0] == "FOO=1",
               "watch lastCompiled owns FOO=1");

    expectTrue(compiler.pollHotReload() == 0u, "unchanged define-watch file does not crash poll");

    expectTrue(writeFile(glslPath, "void main() { /* define watch */ }\n"),
               "define-watch temp shader rewritten");
    expectTrue(compiler.pollHotReload() == 1u, "rewrite recompiles using owned define strings");

    compiled = compiler.lastCompiled(glslUtf8.c_str());
    expectTrue(compiled != nullptr && compiled->valid, "recompile after stack death still valid");
    expectTrue(compiled->defineCount == 1u, "recompile defineCount still 1");
    expectTrue(compiled->defines.size() == 1u && compiled->defines[0] == "FOO=1",
               "recompile still owns FOO=1 after stack death");

    std::error_code ec;
    std::filesystem::remove(glslPath, ec);
    std::filesystem::remove(spvPath, ec);
}

void testWatchCopiesIncludePaths() {
    const std::filesystem::path glslPath = uniqueTempShaderPath();
    const std::string glslUtf8 = glslPath.string();
    const std::filesystem::path spvPath(glslUtf8 + ".spv");

    expectTrue(writeFile(glslPath, "void main() {}\n"), "include-watch temp glsl created");

    const std::string fixtureSpv = fixturePath("minimal.vert.spv");
    std::ifstream spirvIn(fixtureSpv, std::ios::binary);
    expectTrue(spirvIn.good(), "fixture spirv readable for include-watch test");
    std::vector<char> spirvBytes((std::istreambuf_iterator<char>(spirvIn)),
                                 std::istreambuf_iterator<char>());
    expectTrue(!spirvBytes.empty(), "fixture spirv non-empty for include-watch test");

    std::ofstream spirvOut(spvPath, std::ios::binary | std::ios::trunc);
    spirvOut.write(spirvBytes.data(), static_cast<std::streamsize>(spirvBytes.size()));
    spirvOut.flush();
    expectTrue(static_cast<bool>(spirvOut), "sibling spirv written for include-watch test");
    spirvOut.close();

    fuse::renderer::ShaderCompiler compiler;
    {
        const char* stackIncludes[] = {"a"};
        fuse::renderer::ShaderDesc desc{};
        desc.sourcePath = glslUtf8.c_str();
        desc.stage = fuse::renderer::ShaderStage::Vertex;
        desc.includePaths = stackIncludes;
        desc.includePathCount = 1u;
        expectTrue(compiler.watch(desc), "watch copies stack include path strings");
    }

    const fuse::renderer::CompiledShader* compiled = compiler.lastCompiled(glslUtf8.c_str());
    expectTrue(compiled != nullptr && compiled->valid, "watch compile with include paths is valid");
    expectTrue(compiled->includePathCount == 1u, "watch lastCompiled includePathCount is 1");
    expectTrue(compiled->includePaths.size() == 1u && compiled->includePaths[0] == "a",
               "watch lastCompiled owns include path a");

    expectTrue(compiler.pollHotReload() == 0u, "unchanged include-watch file does not crash poll");

    expectTrue(writeFile(glslPath, "void main() { /* include watch */ }\n"),
               "include-watch temp shader rewritten");
    expectTrue(compiler.pollHotReload() == 1u, "rewrite recompiles using owned include path strings");

    compiled = compiler.lastCompiled(glslUtf8.c_str());
    expectTrue(compiled != nullptr && compiled->valid, "recompile after include stack death still valid");
    expectTrue(compiled->includePathCount == 1u, "recompile includePathCount still 1");
    expectTrue(compiled->includePaths.size() == 1u && compiled->includePaths[0] == "a",
               "recompile still owns include path a after stack death");

    std::error_code ec;
    std::filesystem::remove(glslPath, ec);
    std::filesystem::remove(spvPath, ec);
}

void testComputePipeline() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for compute pipeline tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

    auto pipelineLayout = fuse::renderer::PipelineLayout::create(*device);
    expectTrue(pipelineLayout != nullptr, "pipeline layout allocated for compute pipeline");

    fuse::renderer::ComputePipelineDesc missingShaderDesc{};
    missingShaderDesc.layout = pipelineLayout.get();
    missingShaderDesc.debugName = "test_compute_missing_shader";
    auto missingShaderPipeline = fuse::renderer::ComputePipeline::create(*device, missingShaderDesc);
    expectTrue(missingShaderPipeline != nullptr, "compute pipeline allocated without shader");
    expectTrue(!missingShaderPipeline->isValid(), "compute pipeline invalid without shader");
    expectTrue(!missingShaderPipeline->info().message.empty(),
               "missing-shader compute pipeline has honest message");

    auto missingFileModule = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Compute, "missing_compute.spv");
    expectTrue(missingFileModule != nullptr, "missing compute shader module allocated");
    expectTrue(!missingFileModule->isValid(), "missing compute shader module is invalid");
    expectTrue(missingFileModule->stage() == fuse::renderer::ShaderStage::Compute,
               "missing compute shader module preserves Compute stage");

    fuse::renderer::ComputePipelineDesc invalidModuleDesc{};
    invalidModuleDesc.layout = pipelineLayout.get();
    invalidModuleDesc.computeShader = missingFileModule.get();
    auto invalidModulePipeline = fuse::renderer::ComputePipeline::create(*device, invalidModuleDesc);
    expectTrue(invalidModulePipeline != nullptr, "compute pipeline allocated with invalid shader");
    expectTrue(!invalidModulePipeline->isValid(), "compute pipeline invalid with invalid shader");

    const std::string vertPath = fixturePath("minimal.vert.spv");
    auto vertexModule = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Vertex, vertPath.c_str());
    expectTrue(vertexModule != nullptr, "vertex shader module allocated for compute stage check");
    fuse::renderer::ComputePipelineDesc wrongStageDesc{};
    wrongStageDesc.layout = pipelineLayout.get();
    wrongStageDesc.computeShader = vertexModule.get();
    auto wrongStagePipeline = fuse::renderer::ComputePipeline::create(*device, wrongStageDesc);
    expectTrue(wrongStagePipeline != nullptr, "compute pipeline allocated with vertex shader");
    expectTrue(!wrongStagePipeline->isValid(), "compute pipeline invalid with non-compute shader");

    const std::string compPath = fixturePath("minimal.comp.spv");
    auto computeModule = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Compute, compPath.c_str());
    expectTrue(computeModule != nullptr, "compute shader module allocated");
    expectTrue(computeModule->stage() == fuse::renderer::ShaderStage::Compute,
               "compute shader module stage is Compute");

    fuse::renderer::ComputePipelineDesc pipelineDesc{};
    pipelineDesc.layout = pipelineLayout.get();
    pipelineDesc.computeShader = computeModule.get();
    pipelineDesc.localSizeX = 8;
    pipelineDesc.localSizeY = 1;
    pipelineDesc.localSizeZ = 1;
    pipelineDesc.debugName = "test_compute_pipeline";

    auto computePipeline = fuse::renderer::ComputePipeline::create(*device, pipelineDesc);
    expectTrue(computePipeline != nullptr, "compute pipeline allocated");
    expectTrue(computePipeline->info().localSizeX == 8u, "compute pipeline records localSizeX");
    expectTrue(computePipeline->info().localSizeY == 1u, "compute pipeline records localSizeY");
    expectTrue(computePipeline->info().localSizeZ == 1u, "compute pipeline records localSizeZ");

#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady && computeModule->isValid() && pipelineLayout->isValid()) {
        expectTrue(computePipeline->isValid(), "compute pipeline valid with Vulkan device");
        expectTrue(computePipeline->nativeHandle() != nullptr, "compute pipeline has native handle");
        expectTrue(computePipeline->rebuild(), "compute pipeline rebuild succeeds");
        expectTrue(computePipeline->isValid(), "compute pipeline valid after rebuild");
        expectTrue(computePipeline->nativeHandle() != nullptr,
                   "compute pipeline has native handle after rebuild");
        expectTrue(computePipeline->info().rebuildCount == 1u,
                   "compute pipeline rebuildCount increments");
    } else if (!bootstrap->status().deviceReady) {
        expectTrue(!computePipeline->isValid(), "compute pipeline invalid without ICD");
        expectTrue(!computePipeline->rebuild(), "compute pipeline rebuild returns false without ICD");
    } else {
        expectTrue(!computePipeline->isValid(), "compute pipeline invalid without valid inputs");
    }
#else
    expectTrue(!computePipeline->isValid(), "compute pipeline invalid in stub backend");
    expectTrue(computePipeline->nativeHandle() == nullptr, "stub compute pipeline has no native handle");
    expectTrue(!computePipeline->info().message.empty(), "stub compute pipeline has honest message");
    expectTrue(!computePipeline->rebuild(), "stub compute pipeline rebuild stays invalid");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testSpirvIo();
    testOfflineCompiler();
    testOfflineCompilerDefineVariants();
    testOfflineCompilerIncludePathVariants();
    testCookedFuseshaderLoader();
    testCreateFromCompiledShader();
    testShaderModuleAndPipelineLayout();
    testHotReloadPoller();
    testWatchCopiesDefineStrings();
    testWatchCopiesIncludePaths();
    testComputePipeline();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_shader_pipeline: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_shader_pipeline: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
