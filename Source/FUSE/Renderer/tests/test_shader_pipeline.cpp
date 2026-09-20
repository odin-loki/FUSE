#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/shader/shader_compiler.hpp>
#include <fuse/renderer/shader/shader_io.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
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

void testCookedFuseshaderLoader() {
    const std::string spirvPath = fixturePath("minimal.vert.spv");
    std::ifstream spirvIn(spirvPath, std::ios::binary);
    expectTrue(spirvIn.good(), "fixture spirv readable for cooked loader test");

    std::vector<char> spirvBytes((std::istreambuf_iterator<char>(spirvIn)),
                                 std::istreambuf_iterator<char>());
    const std::string cookedPath = "/tmp/fuse_cooked_loader_test.fuseshader";
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

} // namespace

int main() {
    fuse::core::initialize();

    testSpirvIo();
    testOfflineCompiler();
    testCookedFuseshaderLoader();
    testCreateFromCompiledShader();
    testShaderModuleAndPipelineLayout();
    testHotReloadPoller();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_shader_pipeline: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_shader_pipeline: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
