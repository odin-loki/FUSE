#include <fuse/core/init.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/shadow/csm.hpp>
#include <fuse/renderer/shadow/directional_shadow.hpp>
#include <fuse/renderer/shadow/shadow_atlas.hpp>
#include <fuse/renderer/shadow/shadow_pass.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testCascadeLayout() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::GpuFormat;
    using fuse::renderer::ShadowCameraParams;
    using fuse::renderer::kCascadeCount;

    expectTrue(CascadedShadowMapLayout::cascadeCount() == 4u, "four cascades");
    expectTrue(CascadedShadowMapLayout::depthFormat() == GpuFormat::R32Sfloat, "R32F depth maps");

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 0.1f;
    camera.farPlane = 100.f;

    expectTrue(CascadedShadowMapLayout::validateCascadeSplits(desc), "default splits monotonic");

    expectNear(CascadedShadowMapLayout::computeCascadeNearZ(0u, desc, camera), 0.1f, 0.001f,
               "first cascade near equals camera near");
    expectNear(CascadedShadowMapLayout::computeCascadeNearZ(2u, desc, camera),
               CascadedShadowMapLayout::computeCascadeFarZ(1u, desc, camera), 0.001f,
               "cascade near chains from previous far");
    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(0u, desc, camera), 5.095f, 0.01f,
               "first cascade split");
    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(1u, desc, camera), 15.085f, 0.01f,
               "second cascade split");
    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(2u, desc, camera), 40.06f, 0.01f,
               "third cascade split");
    expectNear(CascadedShadowMapLayout::computeCascadeFarZ(3u, desc, camera), 100.f, 0.01f,
               "last cascade reaches far plane");
    expectTrue(desc.cascadeSplits[kCascadeCount - 1u] == 1.0f, "final split reaches far plane fraction");
}

void testCascadeSplitValidation() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;

    CascadedShadowMapDesc validDesc{};
    expectTrue(CascadedShadowMapLayout::validateCascadeSplits(validDesc), "default splits valid");

    CascadedShadowMapDesc invalidDesc{};
    invalidDesc.cascadeSplits[1] = 0.01f;
    expectTrue(!CascadedShadowMapLayout::validateCascadeSplits(invalidDesc), "non-monotonic splits rejected");

    CascadedShadowMapDesc truncatedDesc{};
    truncatedDesc.cascadeSplits[3] = 0.9f;
    expectTrue(!CascadedShadowMapLayout::validateCascadeSplits(truncatedDesc),
               "final split must reach 1.0");
}

void testBatchCascadeFarZs() {
    using fuse::renderer::CascadedShadowMapDesc;
    using fuse::renderer::CascadedShadowMapLayout;
    using fuse::renderer::ShadowCameraParams;

    CascadedShadowMapDesc desc{};
    ShadowCameraParams camera{};
    camera.nearPlane = 0.5f;
    camera.farPlane = 50.f;

    fuse::f32 farZs[fuse::renderer::kCascadeCount]{};
    CascadedShadowMapLayout::computeCascadeFarZs(desc, camera, farZs);

    expectNear(farZs[0], CascadedShadowMapLayout::computeCascadeFarZ(0u, desc, camera), 0.001f,
               "batch far z matches scalar helper");
    expectNear(farZs[3], 50.f, 0.001f, "batch last cascade reaches far plane");
    expectTrue(farZs[1] > farZs[0] && farZs[2] > farZs[1] && farZs[3] > farZs[2],
               "batch far zs monotonic");
}

void testShadowAtlasLayout() {
    using fuse::renderer::ShadowAtlas;
    using fuse::renderer::ShadowAtlasDesc;

    ShadowAtlasDesc desc{};
    desc.cascadeResolution = 1024;
    desc.cascadeCount = 4;
    desc.paddingTexels = 1;

    const auto layout = ShadowAtlas::computeLayout(desc);
    expectTrue(layout.atlasWidth == 2050u, "atlas width tiles cascades horizontally");
    expectTrue(layout.atlasHeight == 2050u, "atlas height tiles cascades vertically");
    expectTrue(layout.cascadeViewports[0].width == 1024u, "cascade 0 viewport width");
    expectTrue(layout.cascadeViewports[3].x == 1025u, "cascade 3 viewport x offset");
}

void testDirectionalShadowAllocation() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for shadow test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

    fuse::renderer::DirectionalShadowDesc desc{};
    desc.csm.resolution = 256;
    desc.atlas.cascadeResolution = 256;

    fuse::renderer::DirectionalShadow shadows;
    expectTrue(shadows.init(resources, desc), "directional shadow initialized");
    expectTrue(shadows.isReady(), "directional shadow ready");
    expectTrue(shadows.atlas().isReady(), "shadow atlas ready");
    expectTrue(shadows.atlas().texture().isValid(), "atlas texture allocated");

    for (fuse::u32 i = 0; i < fuse::renderer::kCascadeCount; ++i) {
        expectTrue(shadows.data().shadowMaps[i].isValid(), "cascade depth map allocated");
    }

    fuse::renderer::ShadowCameraParams camera{};
    camera.position = {0.f, 10.f, 20.f};
    camera.forward = {0.f, -0.2f, -1.f};
    camera.farPlane = 500.f;

    shadows.update(camera, {-0.3f, -1.f, -0.2f});
    expectTrue(shadows.stats().framesUpdated == 1u, "shadow update recorded");
    expectTrue(shadows.data().cascadeFarZ[0] > camera.nearPlane, "first cascade far z computed");
    expectTrue(shadows.data().lightViewProj[0].data[15] != 0.f, "cascade matrix populated");

    shadows.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testShadowPassGraph() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for shadow pass test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::DirectionalShadow shadows;
    shadows.init(resources, {});

    auto pass = fuse::renderer::ShadowPass::create({});
    expectTrue(pass->isReady(), "shadow pass ready");

    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);
    expectTrue(pass->recordFrame(shadows, graph), "shadow pass recorded into graph");
    graph.compile();
    expectTrue(graph.compileInfo().passCount == 1u, "one shadow pass node");
    expectTrue(pass->lastStats().framesRecorded == 1u, "shadow pass stats updated");

    shadows.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testCascadeLayout();
    testCascadeSplitValidation();
    testBatchCascadeFarZs();
    testShadowAtlasLayout();
    testDirectionalShadowAllocation();
    testShadowPassGraph();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_shadow_system: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_shadow_system: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
