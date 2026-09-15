#include <fuse/core/init.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

void testDdgiDescDefaults() {
    fuse::renderer::DDGIDesc desc{};
    expectTrue(desc.grid_dims.x == 16u && desc.grid_dims.y == 8u && desc.grid_dims.z == 16u,
               "default grid is 16x8x16");
    expectTrue(desc.rays_per_probe == 256u, "default rays per probe");
    expectTrue(desc.probes_per_frame == 64u, "default probes per frame");
    expectTrue(fuse::renderer::ddgi_util::probeCount(desc) == 2048u, "2048 probes in default grid");
}

void testProbeGridMath() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {1.f, 2.f, 3.f};
    desc.probe_spacing = {2.f, 2.f, 2.f};
    desc.grid_dims = {2, 2, 2};

    const fuse::math::Vec3 p0 = fuse::renderer::ddgi_util::probeWorldPosition(desc, 0u);
    expectNear(p0.x, 1.f, 1e-5f, "probe 0 x");
    expectNear(p0.y, 2.f, 1e-5f, "probe 0 y");
    expectNear(p0.z, 3.f, 1e-5f, "probe 0 z");

    const fuse::math::Vec3 p7 = fuse::renderer::ddgi_util::probeWorldPosition(desc, 7u);
    expectNear(p7.x, 3.f, 1e-5f, "probe 7 x");
    expectNear(p7.y, 4.f, 1e-5f, "probe 7 y");
    expectNear(p7.z, 5.f, 1e-5f, "probe 7 z");

    expectTrue(fuse::renderer::ddgi_util::irradianceAtlasWidth(desc) == 16u, "irradiance atlas width");
    expectTrue(fuse::renderer::ddgi_util::irradianceAtlasHeight(desc) == 32u, "irradiance atlas height");
}

void testProbeGridIndexing() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {4, 2, 3};

    const fuse::renderer::ProbeGridCoord coord =
        fuse::renderer::ProbeGridLayout::probeCoordFromIndex(desc, 17u);
    expectTrue(coord.x == 1u && coord.y == 0u && coord.z == 2u, "probe index 17 decodes to (1,0,2)");

    const fuse::u32 roundTrip =
        fuse::renderer::ProbeGridLayout::probeIndexFromCoord(desc, coord);
    expectTrue(roundTrip == 17u, "probe coord round-trips to index 17");

    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeIndex(desc, 23u), "last probe index valid");
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeIndex(desc, 24u), "out-of-range probe index");

    const fuse::renderer::ProbeGridCoord invalid{4, 0, 0};
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeCoord(desc, invalid), "x out of range");
}

void testProbeGridWorldCoord() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {2.f, 2.f, 2.f};
    desc.grid_dims = {3, 3, 3};

    const fuse::math::Vec3 gridCoord =
        fuse::renderer::ProbeGridLayout::worldToProbeGridCoord(desc, {4.f, 2.f, 6.f});
    expectNear(gridCoord.x, 2.f, 1e-5f, "world x maps to grid coord 2");
    expectNear(gridCoord.y, 1.f, 1e-5f, "world y maps to grid coord 1");
    expectNear(gridCoord.z, 3.f, 1e-5f, "world z maps to grid coord 3");

    const fuse::renderer::ProbeGridCoord clamped =
        fuse::renderer::ProbeGridLayout::clampProbeGridCoord(desc, {9, 9, 9});
    expectTrue(clamped.x == 2u && clamped.y == 2u && clamped.z == 2u, "clamp to grid max");
}

void testProbeAtlasLayout() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {4, 2, 3};
    desc.irradiance_res = 8;
    desc.depth_res = 16;

    const fuse::renderer::ProbeGridCoord coord{1, 1, 2};
    const fuse::math::Vec2 irradianceOrigin =
        fuse::renderer::ProbeGridLayout::probeIrradianceAtlasOrigin(desc, coord);
    expectNear(irradianceOrigin.x, 8.f, 1e-5f, "irradiance atlas x origin");
    expectNear(irradianceOrigin.y, 40.f, 1e-5f, "irradiance atlas y origin");

    const fuse::math::Vec2 depthOrigin = fuse::renderer::ProbeGridLayout::probeDepthAtlasOrigin(desc, coord);
    expectNear(depthOrigin.x, 16.f, 1e-5f, "depth atlas x origin");
    expectNear(depthOrigin.y, 80.f, 1e-5f, "depth atlas y origin");
}

void testIrradianceLerp() {
    const fuse::math::Vec3 a{1.f, 0.f, 0.f};
    const fuse::math::Vec3 b{0.f, 1.f, 0.f};
    const fuse::math::Vec3 mid = fuse::renderer::ddgi_util::lerpIrradiance(a, b, 0.5f);
    expectNear(mid.x, 0.5f, 1e-5f, "lerp midpoint x");
    expectNear(mid.y, 0.5f, 1e-5f, "lerp midpoint y");
}

void testTrilinearProbeIrradiance() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {static_cast<fuse::f32>(i), 0.f, 0.f};
    }

    const fuse::math::Vec3 centre = fuse::renderer::ddgi_util::trilinearProbeIrradiance(
        desc, {0.5f, 0.5f, 0.5f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    expectNear(centre.x, 3.5f, 1e-4f, "trilinear centre averages eight probes");

    const fuse::math::Vec3 corner = fuse::renderer::ddgi_util::trilinearProbeIrradiance(
        desc, {0.f, 0.f, 0.f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    expectNear(corner.x, 0.f, 1e-5f, "trilinear at probe 0 returns probe 0 irradiance");
}

void testProbeScheduling() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    fuse::renderer::ddgi_util::scheduleProbeUpdates(0u, 2048u, 64u, indices, 64u, &count);
    expectTrue(count == 64u, "schedules 64 probes");
    expectTrue(indices[0] == 0u, "frame 0 starts at probe 0");
    expectTrue(indices[63] == 63u, "frame 0 ends at probe 63");

    fuse::renderer::ddgi_util::scheduleProbeUpdates(1u, 2048u, 64u, indices, 64u, &count);
    expectTrue(indices[0] == 64u, "frame 1 starts at probe 64");
}

void testHysteresisBlend() {
    const fuse::math::Vec3 previous{1.f, 0.f, 0.f};
    const fuse::math::Vec3 incoming{0.f, 1.f, 0.f};
    const fuse::math::Vec3 blended =
        fuse::renderer::ddgi_util::blendIrradiance(previous, incoming, 0.97f);
    expectNear(blended.x, 0.97f, 1e-5f, "hysteresis x");
    expectNear(blended.y, 0.03f, 1e-5f, "hysteresis y");
}

void testDdgiInitUpdateSample() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for DDGI test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready for DDGI");

    fuse::renderer::DDGI ddgi;
    fuse::renderer::DDGIDesc desc{};
    expectTrue(ddgi.init(desc, resources), "DDGI initialises probe volume");
    expectTrue(ddgi.isReady(), "DDGI ready");
    expectTrue(ddgi.volume().irradiance_atlas.isValid(), "irradiance atlas allocated");
    expectTrue(ddgi.volume().depth_atlas.isValid(), "depth atlas allocated");
    expectTrue(ddgi.volume().probe_offsets.isValid(), "probe offsets buffer allocated");
    expectTrue(ddgi.volume().probe_count == 2048u, "probe volume count");

    const fuse::renderer::DdgiInfo info = ddgi.info();
    expectTrue(info.valid, "DDGI info valid");
    expectTrue(info.probe_count == 2048u, "DDGI info probe count");

    expectTrue(ddgi.update(0u), "first DDGI update completes");
    expectTrue(ddgi.lastUpdateStats().probes_scheduled == 64u, "64 probes scheduled");
    expectTrue(ddgi.lastUpdateStats().kernel_launched, "DDGI kernel launch stub succeeds");

    fuse::renderer::DDGISampleRequest sampleRequest{};
    sampleRequest.world_position = fuse::renderer::ddgi_util::probeWorldPosition(desc, 0u);
    const fuse::renderer::DDGISampleResult sample = ddgi.sampleIrradiance(sampleRequest);
    expectTrue(sample.valid, "irradiance sample valid");
    expectTrue(sample.nearest_probe == 0u, "nearest probe at origin cell");

    ddgi.destroy();
    expectTrue(!ddgi.isReady(), "DDGI destroyed");
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testPipelineSlot() {
    expectTrue(std::string(fuse::renderer::DeferredFramePipeline::passName(
                   fuse::renderer::DeferredPassId::DdgiProbeUpdate)) == "ddgi_probe_update",
               "DDGI pass registered in deferred pipeline");
}

void testDdgiInfo() {
    const fuse::renderer::DdgiInfo info = fuse::renderer::ddgi_info();
    expectTrue(info.valid, "ddgi_info valid");
#if defined(FUSE_HAS_CUDA)
    expectTrue(info.backend == fuse::renderer::DdgiBackend::Cuda, "CUDA backend reported when enabled");
#else
    expectTrue(info.backend == fuse::renderer::DdgiBackend::Stub, "stub backend without CUDA");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testDdgiDescDefaults();
    testProbeGridMath();
    testProbeGridIndexing();
    testProbeGridWorldCoord();
    testProbeAtlasLayout();
    testIrradianceLerp();
    testTrilinearProbeIrradiance();
    testProbeScheduling();
    testHysteresisBlend();
    testDdgiInitUpdateSample();
    testPipelineSlot();
    testDdgiInfo();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_ddgi: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ddgi: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
