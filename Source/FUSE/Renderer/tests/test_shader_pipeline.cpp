#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/shader/shader_compiler.hpp>
#include <fuse/renderer/shader/shader_io.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/types.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
}

void testShaderModuleAndPipelineLayout() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchainPlaceholder = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for shader tests");

    const std::string vertPath = fixturePath("minimal.vert.spv");
    fuse::renderer::VulkanDevice* device = bootstrap->device();
    expectTrue(device != nullptr, "device pointer available");

    auto shaderModule = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Vertex, vertPath.c_str());
    expectTrue(shaderModule != nullptr, "shader module allocated");
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
    } else {
        expectTrue(!pipelineLayout->isValid(), "pipeline layout invalid without ICD");
    }
#else
    expectTrue(pipelineLayout->isValid(), "pipeline layout valid in stub backend");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testSpirvIo();
    testOfflineCompiler();
    testShaderModuleAndPipelineLayout();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_shader_pipeline: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_shader_pipeline: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
