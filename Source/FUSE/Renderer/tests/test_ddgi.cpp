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

    const fuse::renderer::ProbeGridCoord origin{0, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::probeIndexFromCoord(desc, origin) == 0u, "origin index 0");
    const fuse::renderer::ProbeGridCoord last{3, 1, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::probeIndexFromCoord(desc, last) == 23u, "last coord index 23");
    expectTrue(fuse::renderer::ProbeGridLayout::probeIndexFromCoord(desc, invalid) == UINT32_MAX,
               "invalid coord returns UINT32_MAX");
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

    const fuse::math::Vec3 oobGrid =
        fuse::renderer::ProbeGridLayout::worldToProbeGridCoord(desc, {100.f, -10.f, 50.f});
    const fuse::math::Vec3 clampedGrid =
        fuse::renderer::ProbeGridLayout::clampWorldToProbeGridCoord(desc, oobGrid);
    expectNear(clampedGrid.x, 2.f, 1e-5f, "OOB grid x clamped to max");
    expectNear(clampedGrid.y, 0.f, 1e-5f, "OOB grid y clamped to min");
    expectNear(clampedGrid.z, 2.f, 1e-5f, "OOB grid z clamped to max");
}

void testProbeIndexClamp() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {4, 2, 3};

    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeIndex(17u, desc) == 17u,
               "in-range probe index unchanged");
    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeIndex(999u, desc) == 23u,
               "OOB probe index clamped to last probe");
    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeIndex(0u, desc) == 0u,
               "origin probe index unchanged");

    const fuse::renderer::ProbeGridCoord clampedCoord =
        fuse::renderer::ProbeGridLayout::clampProbeGridCoord(desc, {9, 9, 9});
    expectTrue(clampedCoord.x == 3u && clampedCoord.y == 1u && clampedCoord.z == 2u,
               "probe coord clamped to grid max per axis");
}

void testEmptyProbeGrid() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {0, 8, 16};

    expectTrue(fuse::renderer::ddgi_util::probeCount(desc) == 0u, "zero-dimension grid has zero probes");
    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeIndex(5u, desc) == 0u,
               "empty grid index clamp returns 0");
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeIndex(desc, 0u), "index 0 invalid on empty grid");

    const fuse::renderer::ProbeValidityFlags emptyFlags =
        fuse::renderer::ProbeGridLayout::probeValidity(desc, {0, 0, 0});
    expectTrue(!emptyFlags.valid, "validity invalid on empty grid");

    fuse::u32 indices[8]{};
    fuse::u32 count = 99u;
    fuse::renderer::ddgi_util::scheduleProbeUpdates(0u, 0u, 64u, indices, 8u, &count);
    expectTrue(count == 0u, "empty grid schedules zero probes");

    const fuse::math::Vec3 emptySample = fuse::renderer::ddgi_util::trilinearProbeIrradiance(
        desc, {0.f, 0.f, 0.f}, nullptr, 0u);
    expectNear(emptySample.x, 0.f, 1e-5f, "empty grid trilinear sample returns zero");
    expectTrue(fuse::renderer::ddgi_util::nearestProbeIndex(desc, {0.f, 0.f, 0.f}) == UINT32_MAX,
               "nearest probe on empty grid is UINT32_MAX");

    const fuse::math::Vec2 texelOffset =
        fuse::renderer::DdgiIrradianceEncoding::directionToTexelOffset({0.f, 1.f, 0.f}, 0u);
    expectNear(texelOffset.x, 0.f, 1e-5f, "zero irradiance_res yields zero texel offset");
}

void testProbeValidityFlags() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {3, 3, 3};

    const fuse::renderer::ProbeGridCoord corner{0, 0, 0};
    const fuse::renderer::ProbeValidityFlags cornerFlags =
        fuse::renderer::ProbeGridLayout::probeValidity(desc, corner);
    expectTrue(cornerFlags.valid, "corner probe valid");
    expectTrue(cornerFlags.is_border, "corner probe is border");
    expectTrue(!cornerFlags.interior, "corner probe not interior");
    expectTrue(!cornerFlags.has_trilinear_neighbourhood, "corner lacks trilinear neighbourhood");

    const fuse::renderer::ProbeGridCoord faceEdge{0, 1, 1};
    const fuse::renderer::ProbeValidityFlags faceFlags =
        fuse::renderer::ProbeGridLayout::probeValidity(desc, faceEdge);
    expectTrue(faceFlags.valid, "face-edge probe valid");
    expectTrue(faceFlags.is_border, "face-edge probe is border");
    expectTrue(!faceFlags.interior, "face-edge probe not interior");
    expectTrue(!faceFlags.has_trilinear_neighbourhood, "face-edge lacks trilinear neighbourhood");

    const fuse::renderer::ProbeGridCoord interior{1, 1, 1};
    const fuse::renderer::ProbeValidityFlags interiorFlags =
        fuse::renderer::ProbeGridLayout::probeValidity(desc, interior);
    expectTrue(interiorFlags.valid, "interior probe valid");
    expectTrue(!interiorFlags.is_border, "interior probe not border");
    expectTrue(interiorFlags.interior, "interior probe flagged interior");
    expectTrue(interiorFlags.has_trilinear_neighbourhood, "interior has trilinear neighbourhood");

    fuse::renderer::DDGIDesc single{};
    single.grid_dims = {1, 1, 1};
    const fuse::renderer::ProbeValidityFlags loneProbe =
        fuse::renderer::ProbeGridLayout::probeValidity(single, {0, 0, 0});
    expectTrue(loneProbe.valid, "1x1x1 lone probe valid");
    expectTrue(loneProbe.is_border, "1x1x1 lone probe is border");
    expectTrue(!loneProbe.has_trilinear_neighbourhood, "1x1x1 lacks trilinear neighbourhood");

    const fuse::renderer::ProbeValidityFlags fromIndex =
        fuse::renderer::ProbeGridLayout::probeValidityFromIndex(desc, 13u);
    expectTrue(fromIndex.valid, "index 13 validity from index helper");
    expectTrue(fromIndex.interior, "index 13 is interior in 3x3x3 grid");

    const fuse::renderer::ProbeValidityFlags invalidIndex =
        fuse::renderer::ProbeGridLayout::probeValidityFromIndex(desc, 99u);
    expectTrue(!invalidIndex.valid, "out-of-range index invalid");
}

void testIrradianceOctahedralEncoding() {
    const fuse::math::Vec3 up{0.f, 1.f, 0.f};
    const fuse::math::Vec2 encoded = fuse::renderer::DdgiIrradianceEncoding::encodeDirection(up);
    const fuse::math::Vec3 decoded = fuse::renderer::DdgiIrradianceEncoding::decodeDirection(encoded);
    const fuse::f32 error = fuse::renderer::DdgiIrradianceEncoding::angularErrorRadians(up, decoded);
    expectTrue(error < 0.001f, "octahedral direction round-trip < 0.001 rad");

    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;
    const fuse::renderer::ProbeGridCoord coord{1, 0, 1};
    const fuse::math::Vec2 texel =
        fuse::renderer::ProbeGridLayout::probeIrradianceAtlasTexel(desc, coord, up);
    const fuse::math::Vec2 origin =
        fuse::renderer::ProbeGridLayout::probeIrradianceAtlasOrigin(desc, coord);
    expectTrue(texel.x >= origin.x && texel.y >= origin.y, "atlas texel within probe tile");
    expectTrue(texel.x < origin.x + static_cast<fuse::f32>(desc.irradiance_res),
               "atlas texel x within tile width");
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

    const fuse::math::Vec3 atA = fuse::renderer::ddgi_util::lerpIrradiance(a, b, 0.f);
    expectNear(atA.x, 1.f, 1e-5f, "lerp t=0 returns a.x");
    expectNear(atA.y, 0.f, 1e-5f, "lerp t=0 returns a.y");

    const fuse::math::Vec3 atB = fuse::renderer::ddgi_util::lerpIrradiance(a, b, 1.f);
    expectNear(atB.x, 0.f, 1e-5f, "lerp t=1 returns b.x");
    expectNear(atB.y, 1.f, 1e-5f, "lerp t=1 returns b.y");

    const fuse::math::Vec3 below = fuse::renderer::ddgi_util::lerpIrradiance(a, b, -0.5f);
    expectNear(below.x, 1.f, 1e-5f, "lerp t<0 clamps to a.x");
    const fuse::math::Vec3 above = fuse::renderer::ddgi_util::lerpIrradiance(a, b, 1.5f);
    expectNear(above.y, 1.f, 1e-5f, "lerp t>1 clamps to b.y");
}

void testBilinearTileIrradiance() {
    const fuse::math::Vec3 samples[4] = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 1.f, 0.f}};
    const fuse::math::Vec3 corner =
        fuse::renderer::ddgi_util::bilinearTileIrradiance(samples, 0.f, 0.f);
    expectNear(corner.x, 0.f, 1e-5f, "bilinear corner u=0 v=0");
    const fuse::math::Vec3 opposite =
        fuse::renderer::ddgi_util::bilinearTileIrradiance(samples, 1.f, 1.f);
    expectNear(opposite.x, 1.f, 1e-5f, "bilinear opposite u=1 v=1");
    const fuse::math::Vec3 centre =
        fuse::renderer::ddgi_util::bilinearTileIrradiance(samples, 0.5f, 0.5f);
    expectNear(centre.x, 0.5f, 1e-5f, "bilinear centre");
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

    const fuse::math::Vec3 maxCorner = fuse::renderer::ddgi_util::trilinearProbeIrradiance(
        desc, {1.f, 1.f, 1.f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    const fuse::math::Vec3 oobHigh = fuse::renderer::ddgi_util::trilinearProbeIrradiance(
        desc, {100.f, 100.f, 100.f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    expectNear(oobHigh.x, maxCorner.x, 1e-4f, "OOB high world position clamps to max-corner sample");

    const fuse::math::Vec3 minCorner = fuse::renderer::ddgi_util::trilinearProbeIrradiance(
        desc, {0.f, 0.f, 0.f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    const fuse::math::Vec3 oobLow = fuse::renderer::ddgi_util::trilinearProbeIrradiance(
        desc, {-100.f, -100.f, -100.f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    expectNear(oobLow.x, minCorner.x, 1e-4f, "OOB low world position clamps to min-corner sample");
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
    testProbeIndexClamp();
    testEmptyProbeGrid();
    testProbeValidityFlags();
    testProbeAtlasLayout();
    testIrradianceOctahedralEncoding();
    testIrradianceLerp();
    testBilinearTileIrradiance();
    testTrilinearProbeIrradiance();
    testProbeScheduling();
    testHysteresisBlend();
    testPipelineSlot();
    testDdgiInfo();
    testDdgiInitUpdateSample();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_ddgi: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ddgi: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
