#include <fuse/core/init.hpp>
#include <fuse/renderer/deferred/deferred_renderer.hpp>
#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/resource_manager.hpp>
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

void testClusterDescCount() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 16;
    desc.tilesY = 9;
    desc.slicesZ = 24;
    expectTrue(desc.clusterCount() == 16u * 9u * 24u, "cluster count product");
}

void testClusterIndex() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;
    expectTrue(fuse::renderer::ClusterGridLayout::clusterIndex(1, 1, 2, desc) == 17u,
               "cluster index layout");
    expectTrue(fuse::renderer::ClusteredLightCuller::clusterIndex(1, 1, 2, desc) == 17u,
               "culler cluster index delegates to grid layout");

    fuse::u32 tileX = 0u;
    fuse::u32 tileY = 0u;
    fuse::u32 sliceZ = 0u;
    fuse::renderer::ClusterGridLayout::decodeClusterIndex(17u, desc, tileX, tileY, sliceZ);
    expectTrue(tileX == 1u && tileY == 1u && sliceZ == 2u, "cluster index decode round-trip");
}

void testClusterGridClampAndScreenMapping() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(fuse::renderer::ClusterGridLayout::clampClusterIndex(999u, desc) == 23u,
               "cluster index clamped to grid bounds");
    expectTrue(fuse::renderer::ClusterGridLayout::clampTileX(99u, desc) == 3u, "tile X clamp");
    expectTrue(fuse::renderer::ClusterGridLayout::clampSliceZ(99u, desc) == 2u, "slice Z clamp");

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    const fuse::u32 midSlice =
        fuse::renderer::ClusterSliceLayout::computeSliceZFromDepth(10.f, desc, camera);
    expectTrue(midSlice < desc.slicesZ, "depth maps into slice range");

    fuse::u32 clusterIndex = 0u;
    expectTrue(fuse::renderer::ClusterGridLayout::mapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, desc, camera, clusterIndex),
               "screen depth maps to cluster index");
    expectTrue(clusterIndex < desc.clusterCount(), "mapped cluster index in bounds");

    fuse::u32 outOfRange = 0u;
    expectTrue(!fuse::renderer::ClusterGridLayout::mapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 0.01f, desc, camera, outOfRange),
               "depth below near plane rejected");
}

void testSliceDepthDistribution() {
    fuse::renderer::ClusterDesc desc{};
    desc.slicesZ = 4;

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    const fuse::f32 slice0Near =
        fuse::renderer::ClusterSliceLayout::computeSliceNearZ(0u, desc, camera);
    const fuse::f32 slice0Far = fuse::renderer::ClusterSliceLayout::computeSliceFarZ(0u, desc, camera);
    const fuse::f32 slice3Far = fuse::renderer::ClusterSliceLayout::computeSliceFarZ(3u, desc, camera);

    expectNear(slice0Near, 1.f, 0.001f, "first slice starts at near plane");
    expectTrue(slice0Far > slice0Near, "slice far exceeds near");
    expectNear(slice3Far, 100.f, 0.001f, "last slice reaches far plane");
}

void testLightGridRebuildOverflowClamp() {
    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = 2u;
    grid.grid.resize(clusterCount);

    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u, 2u, 3u},
        {4u, 5u},
    };

    const fuse::u32 dropped = fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(
        grid, clusterCount, perClusterLights, 2u);

    expectTrue(dropped == 2u, "overflow lights dropped during rebuild");
    expectTrue(grid.grid[0].count == 2u, "cluster 0 count clamped");
    expectTrue(grid.lightList.size() == 4u, "flat list respects per-cluster cap");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsets(grid, clusterCount),
               "clamped grid offsets remain contiguous");
}

void testLightGridRebuildLayout() {
    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = 3u;
    grid.grid.resize(clusterCount);

    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 2u},
        {},
        {1u},
    };

    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights);

    expectTrue(grid.lightList.size() == 3u, "flat light list packed");
    expectTrue(grid.grid[0].offset == 0u && grid.grid[0].count == 2u, "cluster 0 offset/count");
    expectTrue(grid.grid[1].offset == 2u && grid.grid[1].count == 0u, "empty cluster offset preserved");
    expectTrue(grid.grid[2].offset == 2u && grid.grid[2].count == 1u, "cluster 2 offset/count");
    expectTrue(grid.lightList[0] == 0u && grid.lightList[1] == 2u && grid.lightList[2] == 1u,
               "light list ordering");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsets(grid, clusterCount),
               "rebuilt grid offsets contiguous");
}

void testCullerInitAndClusterBuild() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for cluster culler test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 2;
    desc.maxLightsPerCluster = 8;

    fuse::renderer::ClusteredLightCuller culler;
    culler.init(desc, resources);
    expectTrue(culler.isReady(), "cluster culler initialized");
    expectTrue(culler.buffers().clusterCount == 8u, "gpu cluster count");

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 0.5f;
    camera.farPlane = 100.f;
    culler.updateClusters(camera);
    expectTrue(culler.stats().clustersBuilt == 8u, "clusters built");
    expectTrue(culler.gridSoA().aabbs.size() == 8u, "cpu aabb soa size");
    expectTrue(culler.gridSoA().grid.size() == 8u, "cpu grid soa size");

    culler.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testLightCullAssignsAndSkips() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for light cull test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 2;
    desc.maxLightsPerCluster = 8;

    fuse::renderer::ClusteredLightCuller culler;
    culler.init(desc, resources);

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 50.f;

    fuse::renderer::PointLightInput nearLight{};
    nearLight.position = {0.f, 0.f, -5.f};
    nearLight.radius = 2.f;

    fuse::renderer::PointLightInput farLight{};
    farLight.position = {10000.f, 10000.f, -10000.f};
    farLight.radius = 1.f;

    culler.cullLights({nearLight, farLight}, {}, camera);
    expectTrue(culler.stats().lightListEntries > 0u, "near light assigned to clusters");
    expectTrue(culler.stats().lightsCulled > 0u, "culled light count non-zero");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsets(culler.gridSoA(),
                                                                                 desc.clusterCount()),
               "culler light grid offsets contiguous");

    culler.rebuildLightGrid();
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsets(culler.gridSoA(),
                                                                                 desc.clusterCount()),
               "explicit rebuild preserves contiguous offsets");

    fuse::u32 emptyClusters = 0;
    for (const fuse::renderer::ClusterGridEntry& entry : culler.gridSoA().grid) {
        if (entry.count == 0u) {
            ++emptyClusters;
        }
    }
    expectTrue(emptyClusters > 0u, "some clusters have zero lights");

    culler.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testEmptySceneCull() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for empty scene cull test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 2;
    desc.maxLightsPerCluster = 4;

    fuse::renderer::ClusteredLightCuller culler;
    culler.init(desc, resources);

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 50.f;

    culler.cullLights({}, {}, camera);
    expectTrue(culler.stats().lightListEntries == 0u, "empty scene has zero light list entries");
    expectTrue(culler.stats().lightsCulled == 0u, "empty scene culls zero lights");
    expectTrue(culler.stats().lightsDroppedOverflow == 0u, "empty scene has no overflow");
    expectTrue(culler.stats().clustersAtCapacity == 0u, "empty scene has no full clusters");

    fuse::u32 emptyClusters = 0u;
    for (const fuse::renderer::ClusterGridEntry& entry : culler.gridSoA().grid) {
        if (entry.count == 0u) {
            ++emptyClusters;
        }
    }
    expectTrue(emptyClusters == desc.clusterCount(), "all clusters empty with no lights");

    culler.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testLightCullCapacityClamp() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for capacity clamp test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 1;
    desc.tilesY = 1;
    desc.slicesZ = 1;
    desc.maxLightsPerCluster = 2;

    fuse::renderer::ClusteredLightCuller culler;
    culler.init(desc, resources);

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 50.f;

    std::vector<fuse::renderer::PointLightInput> lights;
    for (fuse::u32 i = 0; i < 5u; ++i) {
        fuse::renderer::PointLightInput light{};
        light.position = {0.f, 0.f, -5.f};
        light.radius = 100.f;
        lights.push_back(light);
    }

    culler.cullLights(lights, {}, camera);
    expectTrue(culler.gridSoA().grid[0].count == 2u, "cluster stores at most maxLightsPerCluster");
    expectTrue(culler.stats().lightsDroppedOverflow == 3u, "excess intersecting lights dropped");
    expectTrue(culler.stats().clustersAtCapacity == 1u, "single cluster reported at capacity");

    culler.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testDeferredPipelineWiresClusterPass() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for deferred cluster pass test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::DeferredRendererDesc desc{};
    desc.pipeline.width = 64;
    desc.pipeline.height = 64;

    auto renderer = fuse::renderer::DeferredRenderer::create(desc);
    expectTrue(renderer->init(resources), "deferred renderer with cluster culler initialized");
    expectTrue(renderer->lightCuller().isReady(), "deferred renderer owns cluster culler");

    fuse::renderer::RenderGraph graph;
    expectTrue(renderer->buildFrameGraph(graph, 0u), "deferred frame graph with cluster pass");
    expectTrue(graph.compileInfo().passCount == fuse::renderer::DeferredFramePipeline::passCount(),
               "cluster pass present in deferred schedule");

    renderer->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testClusterDescCount();
    testClusterIndex();
    testClusterGridClampAndScreenMapping();
    testSliceDepthDistribution();
    testLightGridRebuildLayout();
    testLightGridRebuildOverflowClamp();
    testCullerInitAndClusterBuild();
    testLightCullAssignsAndSkips();
    testEmptySceneCull();
    testLightCullCapacityClamp();
    testDeferredPipelineWiresClusterPass();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_clustered_light_culler: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_clustered_light_culler: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
