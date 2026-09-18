#include <fuse/core/init.hpp>
#include <fuse/renderer/deferred/deferred_renderer.hpp>
#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/postprocess/lens_flare.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/volumetric/light_shafts.hpp>
#include <fuse/renderer/volumetric/volumetric_fog.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (expected %.4f, got %.4f)\n", message, expected, actual);
        ++g_failures;
    }
}

void testVolumetricFogDensity() {
    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.height_falloff = 0.2f;
    params.base_height = 0.f;

    const fuse::f32 atBase =
        fuse::renderer::sample_volumetric_fog_density(params, {0.f, 0.f, 0.f});
    const fuse::f32 elevated =
        fuse::renderer::sample_volumetric_fog_density(params, {0.f, 10.f, 0.f});

    expectNear(atBase, 0.02f, 1e-5f, "fog density at base height");
    expectTrue(elevated < atBase, "fog density falls off with height");

    fuse::renderer::VolumetricFogPassStats stats{};
    expectTrue(fuse::renderer::record_volumetric_fog_pass(params, stats), "fog pass records");
    expectTrue(stats.ready, "fog pass stats ready");
    expectTrue(stats.framesRecorded == 1u, "fog pass increments frame count");
}

void testFroxelGridIndexing() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;
    expectTrue(desc.froxelCount() == 24u, "froxel count product");

    const fuse::u32 index = fuse::renderer::FroxelGridLayout::froxelIndex(1u, 1u, 2u, desc);
    expectTrue(index == 17u, "froxel index layout");

    fuse::u32 tileX = 0u;
    fuse::u32 tileY = 0u;
    fuse::u32 sliceZ = 0u;
    fuse::renderer::FroxelGridLayout::decodeFroxelIndex(index, desc, tileX, tileY, sliceZ);
    expectTrue(tileX == 1u && tileY == 1u && sliceZ == 2u, "froxel index decode round-trip");

    expectTrue(fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, 0u, desc) == 0u, "origin froxel index");
    expectTrue(fuse::renderer::FroxelGridLayout::froxelIndex(3u, 1u, 2u, desc) == 23u, "last froxel index");

    fuse::u32 oversizedTileX = 0u;
    fuse::u32 oversizedTileY = 0u;
    fuse::u32 oversizedSliceZ = 0u;
    fuse::renderer::FroxelGridLayout::decodeFroxelIndex(999u, desc, oversizedTileX, oversizedTileY, oversizedSliceZ);
    expectTrue(oversizedTileX == 3u && oversizedTileY == 1u && oversizedSliceZ == 2u,
               "decode clamps oversized froxel index");

    expectTrue(fuse::renderer::FroxelGridLayout::froxelIndexClamped(99u, 99u, 99u, desc) == 23u,
               "clamped froxel index maps OOB coords to last cell");
    expectTrue(fuse::renderer::FroxelGridLayout::froxelIndexClamped(1u, 1u, 2u, desc) == index,
               "clamped froxel index preserves in-bounds coords");
}

void testFroxelGridClampAndCountLimits() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(fuse::renderer::FroxelGridLayout::clampFroxelIndex(999u, desc) == 23u,
               "froxel index clamped to grid bounds");
    expectTrue(fuse::renderer::FroxelGridLayout::clampTileX(99u, desc) == 3u, "tile X clamp");
    expectTrue(fuse::renderer::FroxelGridLayout::clampTileY(99u, desc) == 1u, "tile Y clamp");
    expectTrue(fuse::renderer::FroxelGridLayout::clampSliceZ(99u, desc) == 2u, "slice Z clamp");

    fuse::renderer::FroxelGridDesc oversized{};
    oversized.tilesX = 999u;
    oversized.tilesY = 999u;
    oversized.slicesZ = 999u;
    const fuse::renderer::FroxelGridDesc clamped = fuse::renderer::FroxelGridDesc::clampCounts(oversized);
    expectTrue(clamped.tilesX == fuse::renderer::FroxelGridDesc::kMaxTilesX, "tilesX clamped to max");
    expectTrue(clamped.tilesY == fuse::renderer::FroxelGridDesc::kMaxTilesY, "tilesY clamped to max");
    expectTrue(clamped.slicesZ == fuse::renderer::FroxelGridDesc::kMaxSlicesZ, "slicesZ clamped to max");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    const fuse::u32 midSlice =
        fuse::renderer::FroxelSliceLayout::computeSliceZFromDepth(10.f, desc, camera);
    expectTrue(midSlice < desc.slicesZ, "depth maps into slice range");

    fuse::u32 froxelIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::mapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, desc, camera, froxelIndex),
               "screen depth maps to froxel index");
    expectTrue(froxelIndex < desc.froxelCount(), "mapped froxel index in bounds");

    fuse::u32 belowNear = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::mapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 0.01f, desc, camera, belowNear),
               "depth below near plane rejected for froxel index");

    fuse::u32 aboveFar = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::mapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 200.f, desc, camera, aboveFar),
               "depth above far plane rejected for froxel index");

    fuse::u32 clampedScreen = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::mapScreenDepthToFroxelIndex(
                   -0.5f, 1.5f, 10.f, desc, camera, clampedScreen),
               "out-of-range screen coords clamp to froxel grid");
    expectTrue(clampedScreen < desc.froxelCount(), "clamped screen maps to valid froxel");
}

void testFroxelSliceDepthDistribution() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.slicesZ = 4;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    const fuse::f32 slice0Near =
        fuse::renderer::FroxelSliceLayout::computeSliceNearZ(0u, desc, camera);
    const fuse::f32 slice0Far = fuse::renderer::FroxelSliceLayout::computeSliceFarZ(0u, desc, camera);
    const fuse::f32 slice3Far = fuse::renderer::FroxelSliceLayout::computeSliceFarZ(3u, desc, camera);

    expectNear(slice0Near, 1.f, 0.001f, "first froxel slice starts at near plane");
    expectTrue(slice0Far > slice0Near, "froxel slice far exceeds near");
    expectNear(slice3Far, 100.f, 0.001f, "last froxel slice reaches far plane");
}

void testFroxelDensityLerpHelpers() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 2;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.density.assign(desc.froxelCount(), 0.f);
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, 0u, desc)] = 0.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 0u, 0u, desc)] = 1.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 1u, 0u, desc)] = 0.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 1u, 0u, desc)] = 1.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, 1u, desc)] = 0.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 0u, 1u, desc)] = 1.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 1u, 1u, desc)] = 0.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 1u, 1u, desc)] = 1.5f;

    expectNear(fuse::renderer::froxel_util::lerpDensity(0.f, 1.f, 0.5f), 0.5f, 1e-5f, "density lerp midpoint");
    expectNear(fuse::renderer::froxel_util::lerpDensity(2.f, 4.f, 0.f), 2.f, 1e-5f, "density lerp at t=0");
    expectNear(fuse::renderer::froxel_util::lerpDensity(2.f, 4.f, 1.f), 4.f, 1e-5f, "density lerp at t=1");
    expectNear(fuse::renderer::froxel_util::lerpDensity(0.f, 1.f, -1.f), 0.f, 1e-5f, "density lerp clamps low t");
    expectNear(fuse::renderer::froxel_util::lerpDensity(0.f, 1.f, 2.f), 1.f, 1e-5f, "density lerp clamps high t");

    fuse::renderer::FroxelSampleCoords coords{};
    coords.tileX0 = 0u;
    coords.tileY0 = 0u;
    coords.tileX1 = 1u;
    coords.tileY1 = 1u;
    coords.sliceZ0 = 0u;
    coords.sliceZ1 = 1u;
    coords.tx = 0.5f;
    coords.ty = 0.5f;
    coords.tz = 0.5f;

    const fuse::f32 bilinear =
        fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, coords);
    expectNear(bilinear, 0.5f, 1e-5f, "bilinear froxel density sample");

    const fuse::f32 trilinear =
        fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, coords);
    expectNear(trilinear, 0.75f, 1e-5f, "trilinear froxel density sample");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::mapScreenDepthToSampleCoords(
                   0.25f, 0.5f, 10.f, desc, camera, mapped),
               "screen depth maps to froxel sample coords");
    expectTrue(mapped.tileX0 < desc.tilesX && mapped.tileY0 < desc.tilesY, "mapped tile coords in range");
    expectTrue(mapped.sliceZ0 < desc.slicesZ, "mapped slice in range");

    fuse::u32 rejectedDepth = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::mapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, mapped),
               "sample coords reject depth below near plane");
    expectTrue(!fuse::renderer::FroxelGridLayout::mapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 200.f, desc, camera, rejectedDepth),
               "froxel index rejects depth above far plane");

    fuse::renderer::FroxelSampleCoords mappedScreen{};
    expectTrue(fuse::renderer::FroxelGridLayout::mapScreenDepthToSampleCoords(
                   0.25f, 0.25f, 3.16f, desc, camera, mappedScreen),
               "screen coords map for density sample");
    const fuse::f32 expectedScreen =
        fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, mappedScreen);
    const fuse::f32 screenSample = fuse::renderer::froxel_util::sampleDensityAtScreen(
        grid, desc, camera, 0.25f, 0.25f, 3.16f);
    expectNear(screenSample, expectedScreen, 1e-5f, "screen-space trilinear density sample");

    fuse::renderer::FroxelGridDesc zeroGrid{};
    zeroGrid.tilesX = 0u;
    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectNear(fuse::renderer::froxel_util::sampleDensityAtScreen(emptyGrid, zeroGrid, camera, 0.5f, 0.5f, 10.f),
               0.f,
               1e-6f,
               "empty grid screen sample returns zero");
}

void testEmptySceneVolumetricFog() {
    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.f;

    fuse::renderer::VolumetricFogPassStats stats{};
    expectTrue(!fuse::renderer::record_volumetric_fog_pass(params, stats), "zero-density fog skips pass");
    expectTrue(!stats.ready, "empty scene fog stats not ready");
    expectNear(stats.lastDensity, 0.f, 1e-6f, "empty scene fog density is zero");
    expectTrue(stats.framesRecorded == 0u, "empty scene fog does not advance frame count");

    fuse::renderer::VolumetricFogParams zeroMarch{};
    zeroMarch.density = 0.02f;
    zeroMarch.march_steps = 0u;
    fuse::renderer::VolumetricFogPassStats zeroMarchStats{};
    expectTrue(!fuse::renderer::record_volumetric_fog_pass(zeroMarch, zeroMarchStats),
               "zero march steps skip fog pass");
    expectTrue(!zeroMarchStats.ready, "zero march steps leave stats not ready");

    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 2;
    fuse::renderer::FroxelCameraDesc camera{};
    fuse::renderer::FroxelDensityGrid grid{};
    fuse::renderer::froxel_util::populateFromAnalyticFog(grid, desc, camera, params);
    expectTrue(grid.density.size() == desc.froxelCount(), "empty scene populate allocates grid");
    expectTrue(grid.density.front() == 0.f && grid.density.back() == 0.f, "empty scene populate leaves zero density");
    expectTrue(grid.matchesDesc(desc), "empty scene populate matches desc");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(grid) == 0u,
               "empty scene populate leaves all froxels at zero density");
    expectTrue(fuse::renderer::froxel_util::countEmptyFroxels(grid) == desc.froxelCount(),
               "empty scene populate counts all froxels empty");

    fuse::renderer::FroxelDensityGrid allocated{};
    allocated.allocate(desc);
    expectTrue(allocated.density.size() == desc.froxelCount(), "allocate sizes density buffer");
    expectTrue(allocated.matchesDesc(desc), "allocate matches desc");
    expectTrue(!allocated.isEmpty(), "allocated froxel grid is non-empty");
    expectTrue(fuse::renderer::froxel_util::countEmptyFroxels(allocated) == desc.froxelCount(),
               "fresh allocate reports all froxels empty");

    allocated.clear();
    expectTrue(allocated.isEmpty(), "cleared froxel grid is empty");
    expectTrue(!allocated.matchesDesc(desc), "cleared grid no longer matches desc");

    fuse::renderer::FroxelGridDesc zeroGrid{};
    zeroGrid.tilesX = 0u;
    fuse::renderer::FroxelDensityGrid emptyGrid{};
    fuse::renderer::froxel_util::populateFromAnalyticFog(emptyGrid, zeroGrid, camera, params);
    expectTrue(emptyGrid.density.empty(), "zero-dimension grid stays empty");
    emptyGrid.allocate(zeroGrid);
    expectTrue(emptyGrid.isEmpty(), "allocate on zero-dimension grid stays empty");

    fuse::renderer::FroxelSampleCoords coords{};
    coords.sliceZ0 = 0u;
    expectNear(fuse::renderer::froxel_util::sampleDensityTrilinear(emptyGrid, zeroGrid, coords), 0.f, 1e-6f,
               "empty grid sample returns zero");

    fuse::u32 zeroIndex = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::mapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, zeroGrid, camera, zeroIndex),
               "zero-dimension grid rejects screen mapping");
}

void testFroxelPopulateFromAnalyticFog() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 4;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.height_falloff = 0.2f;
    params.base_height = 0.f;

    fuse::renderer::FroxelDensityGrid grid{};
    fuse::renderer::froxel_util::populateFromAnalyticFog(grid, desc, camera, params);

    expectTrue(grid.density.size() == desc.froxelCount(), "analytic populate fills froxel grid");

    const fuse::u32 nearSlice = fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, 0u, desc);
    const fuse::u32 farSlice = fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, desc.slicesZ - 1u, desc);
    expectTrue(grid.density[nearSlice] >= grid.density[farSlice],
               "analytic populate preserves height falloff across slices");
    expectTrue(grid.matchesDesc(desc), "populated grid matches desc");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(grid) == desc.froxelCount(),
               "analytic populate marks all froxels non-zero");
    expectTrue(fuse::renderer::froxel_util::countEmptyFroxels(grid) == 0u,
               "analytic populate leaves no empty froxels");
}

void testFroxelIndexClampAndLerpGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(!fuse::renderer::FroxelGridLayout::isEmptyGrid(desc), "non-zero froxel grid is not empty");
    expectTrue(fuse::renderer::FroxelGridLayout::isValidFroxelIndex(0u, desc), "origin index is valid");
    expectTrue(fuse::renderer::FroxelGridLayout::isValidFroxelIndex(23u, desc), "last index is valid");
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidFroxelIndex(24u, desc), "index at count is invalid");
    expectTrue(fuse::renderer::FroxelGridLayout::isFroxelIndexOutOfRange(24u, desc),
               "index at count is out of range");
    expectTrue(!fuse::renderer::FroxelGridLayout::isFroxelIndexOutOfRange(23u, desc),
               "last index is in range");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::isEmptyGrid(zeroDesc), "zero x dimension is empty grid");
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidFroxelIndex(0u, zeroDesc),
               "index 0 invalid on empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::isFroxelIndexOutOfRange(0u, zeroDesc),
               "any index out of range on empty grid");

    expectNear(fuse::renderer::froxel_util::lerpDensity(3.f, 3.f, 0.5f), 3.f, 1e-5f,
               "density lerp with equal endpoints");
    expectNear(fuse::renderer::froxel_util::lerpDensity(0.f, 0.f, 2.f), 0.f, 1e-5f,
               "density lerp equal endpoints ignores out-of-range t");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;
    grid.density[23] = 2.f;

    expectTrue(fuse::renderer::froxel_util::gridMatchesDesc(grid, desc), "grid matches desc");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtIndex(grid, desc, 0u), 1.f, 1e-5f,
               "sample at origin index");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtIndex(grid, desc, 23u), 2.f, 1e-5f,
               "sample at last index");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtIndex(grid, desc, 999u), 2.f, 1e-5f,
               "sample clamps OOB index to last froxel");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::gridMatchesDesc(grid, mismatched),
               "grid does not match smaller desc");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtIndex(grid, mismatched, 0u), 0.f, 1e-6f,
               "sample rejects desc mismatch");

    fuse::renderer::FroxelSampleCoords coords{};
    coords.tileX0 = 99u;
    coords.tileY0 = 99u;
    coords.tileX1 = 99u;
    coords.tileY1 = 99u;
    coords.sliceZ0 = 99u;
    coords.sliceZ1 = 99u;
    coords.tx = 2.f;
    coords.ty = -1.f;
    coords.tz = 3.f;
    fuse::renderer::FroxelGridLayout::clampSampleCoords(coords, desc);
    expectTrue(coords.tileX0 == 3u && coords.tileY0 == 1u && coords.sliceZ0 == 2u,
               "clamp sample coords clamps tile/slice corners");
    expectTrue(coords.tileX1 == 3u && coords.tileY1 == 1u && coords.sliceZ1 == 2u,
               "clamp sample coords clamps upper corners");
    expectNear(coords.tx, 1.f, 1e-5f, "clamp sample coords clamps tx high");
    expectNear(coords.ty, 0.f, 1e-5f, "clamp sample coords clamps ty low");
    expectNear(coords.tz, 1.f, 1e-5f, "clamp sample coords clamps tz high");

    fuse::renderer::FroxelSampleCoords unclampedCoords{};
    unclampedCoords.tileX0 = 0u;
    unclampedCoords.tileY0 = 0u;
    unclampedCoords.tileX1 = 1u;
    unclampedCoords.tileY1 = 1u;
    unclampedCoords.sliceZ0 = 0u;
    unclampedCoords.sliceZ1 = 1u;
    unclampedCoords.tx = 2.f;
    unclampedCoords.ty = -1.f;
    unclampedCoords.tz = 3.f;
    const fuse::f32 clampedTrilinear =
        fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, unclampedCoords);
    fuse::renderer::FroxelGridLayout::clampSampleCoords(unclampedCoords, desc);
    const fuse::f32 explicitClamped =
        fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, unclampedCoords);
    expectNear(clampedTrilinear, explicitClamped, 1e-5f,
               "trilinear sample clamps OOB interpolation weights via lerpDensity");
    expectNear(fuse::renderer::froxel_util::sampleDensityTrilinear(grid, mismatched, coords), 0.f, 1e-6f,
               "trilinear sample rejects desc mismatch");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::gridMatchesDesc(emptyGrid, desc),
               "empty storage does not match desc");
    expectNear(fuse::renderer::froxel_util::sampleDensityBilinear(emptyGrid, desc, coords), 0.f, 1e-6f,
               "bilinear sample on empty grid returns zero");
}

void testFroxelGridDensityValidationAndCoordGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);

    expectTrue(fuse::renderer::froxel_util::isDensityGridAccessible(grid, desc),
               "allocated grid is accessible");
    expectTrue(!fuse::renderer::froxel_util::hasNonZeroDensity(grid),
               "fresh allocate has no non-zero density");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelMarch(grid, desc),
               "uniformly zero grid skips froxel march");
    expectTrue(fuse::renderer::froxel_util::validateGridDensity(grid, desc),
               "fresh allocate validates grid density");

    expectTrue(fuse::renderer::froxel_util::writeDensityAtCoord(grid, desc, 1u, 1u, 2u, 3.5f),
               "write at clamped tile coords succeeds");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtCoord(grid, desc, 1u, 1u, 2u), 3.5f, 1e-5f,
               "read at tile coords matches write");

    expectTrue(fuse::renderer::froxel_util::writeDensityAtCoord(grid, desc, 3u, 1u, 2u, 4.5f),
               "write at last tile coords succeeds");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtCoord(grid, desc, 99u, 99u, 99u), 4.5f, 1e-5f,
               "read clamps OOB tile coords to last cell");
    expectTrue(fuse::renderer::froxel_util::hasNonZeroDensity(grid),
               "partial fill reports non-zero density");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelMarch(grid, desc),
               "partially filled grid does not skip march");
    expectTrue(fuse::renderer::froxel_util::validateGridDensity(grid, desc),
               "partially filled grid validates grid density");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::isDensityGridAccessible(grid, mismatched),
               "inaccessible when desc mismatches storage");
    expectTrue(!fuse::renderer::froxel_util::validateGridDensity(grid, mismatched),
               "grid density validation rejects desc mismatch");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtCoord(grid, mismatched, 0u, 0u, 0u), 0.f, 1e-6f,
               "coord sample rejects desc mismatch");
    expectTrue(!fuse::renderer::froxel_util::writeDensityAtCoord(grid, mismatched, 0u, 0u, 0u, 9.f),
               "coord write rejects desc mismatch");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::froxel_util::isDensityGridAccessible(grid, zeroDesc),
               "zero-dimension desc is not accessible");
    expectTrue(fuse::renderer::froxel_util::validateGridDensity(grid, zeroDesc),
               "empty desc vacuously validates grid density");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelMarch(grid, zeroDesc),
               "inaccessible desc skips froxel march");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::isDensityGridAccessible(emptyGrid, desc),
               "empty storage is not accessible");
    expectTrue(!fuse::renderer::froxel_util::validateGridDensity(emptyGrid, desc),
               "empty storage fails grid density validation");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelMarch(emptyGrid, desc),
               "empty storage skips froxel march");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtCoord(emptyGrid, desc, 0u, 0u, 0u), 0.f, 1e-6f,
               "coord sample on empty storage returns zero");
    expectTrue(!fuse::renderer::froxel_util::writeDensityAtCoord(emptyGrid, desc, 0u, 0u, 0u, 1.f),
               "coord write on empty storage rejected");

    fuse::renderer::FroxelDensityGrid undersized{};
    undersized.density.resize(desc.froxelCount() - 1u, 0.f);
    expectTrue(!fuse::renderer::froxel_util::validateGridDensity(undersized, desc),
               "undersized storage fails grid density validation");
}

void testFroxelDensityCountValidationAndWriteGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(fuse::renderer::FroxelGridLayout::maxFroxelIndex(desc) == 23u,
               "max froxel index matches last cell");
    expectTrue(fuse::renderer::FroxelGridLayout::clampFroxelIndex(999u, desc) ==
                   fuse::renderer::FroxelGridLayout::maxFroxelIndex(desc),
               "clamp froxel index agrees with max index");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::maxFroxelIndex(zeroDesc) == 0u,
               "max froxel index on empty grid is zero");

    fuse::renderer::FroxelDensityGrid grid{};
    expectTrue(fuse::renderer::froxel_util::validateDensityCounts(grid),
               "empty storage vacuously validates density counts");

    grid.allocate(desc);
    expectTrue(fuse::renderer::froxel_util::validateDensityCounts(grid),
               "fresh allocate validates density counts");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(grid) +
                       fuse::renderer::froxel_util::countEmptyFroxels(grid) ==
                   desc.froxelCount(),
               "non-zero and empty froxel counts partition grid");

    expectTrue(fuse::renderer::froxel_util::writeDensityAtIndex(grid, desc, 0u, 1.5f),
               "write at origin index succeeds");
    expectTrue(fuse::renderer::froxel_util::writeDensityAtIndex(grid, desc, 999u, 2.5f),
               "write at OOB index clamps and succeeds");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtIndex(grid, desc, 0u), 1.5f, 1e-5f,
               "write at origin readable via sample");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtIndex(grid, desc, 23u), 2.5f, 1e-5f,
               "write at clamped last index readable via sample");
    expectTrue(fuse::renderer::froxel_util::validateDensityCounts(grid),
               "partially filled grid still validates density counts");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(grid) == 2u,
               "two written froxels counted non-zero");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::writeDensityAtIndex(grid, mismatched, 0u, 9.f),
               "write rejects desc mismatch");
    expectTrue(!fuse::renderer::froxel_util::writeDensityAtIndex(grid, zeroDesc, 0u, 9.f),
               "write rejects empty froxel desc");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::writeDensityAtIndex(emptyGrid, desc, 0u, 1.f),
               "write rejects empty density storage");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    expectNear(fuse::renderer::froxel_util::sampleDensityAtScreen(grid, mismatched, camera, 0.5f, 0.5f, 10.f),
               0.f,
               1e-6f,
               "screen sample rejects desc mismatch");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtScreen(grid, zeroDesc, camera, 0.5f, 0.5f, 10.f),
               0.f,
               1e-6f,
               "screen sample rejects empty froxel desc");

    fuse::renderer::FroxelSampleCoords extremeCoords{};
    extremeCoords.tileX0 = 99u;
    extremeCoords.tileY0 = 99u;
    extremeCoords.tileX1 = 99u;
    extremeCoords.tileY1 = 99u;
    extremeCoords.sliceZ0 = 99u;
    extremeCoords.sliceZ1 = 99u;
    extremeCoords.tx = 2.f;
    extremeCoords.ty = -1.f;
    extremeCoords.tz = 3.f;
    const fuse::f32 bilinearExtreme =
        fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, extremeCoords);
    fuse::renderer::FroxelGridLayout::clampSampleCoords(extremeCoords, desc);
    const fuse::f32 bilinearClamped =
        fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, extremeCoords);
    expectNear(bilinearExtreme, bilinearClamped, 1e-5f,
               "bilinear sample clamps OOB interpolation weights internally");
}

void testFroxelMaxIndexDeepenHelpers() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(desc.maxFroxelIndex() == 23u, "FroxelGridDesc maxFroxelIndex matches last cell");
    expectTrue(fuse::renderer::FroxelGridLayout::maxFroxelIndex(desc) == desc.maxFroxelIndex(),
               "FroxelGridLayout maxFroxelIndex delegates to FroxelGridDesc");

    expectTrue(fuse::renderer::FroxelGridLayout::isAtMaxFroxelIndex(23u, desc),
               "last froxel index is at max");
    expectTrue(!fuse::renderer::FroxelGridLayout::isAtMaxFroxelIndex(22u, desc),
               "penultimate froxel index is not at max");
    expectTrue(!fuse::renderer::FroxelGridLayout::isAtMaxFroxelIndex(23u, {}),
               "max index check rejects empty grid");

    fuse::u32 clampedIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampFroxelIndex(17u, desc, clampedIndex),
               "tryClamp succeeds on non-empty grid");
    expectTrue(clampedIndex == 17u, "tryClamp preserves in-bounds index");

    fuse::u32 clampedOob = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampFroxelIndex(999u, desc, clampedOob),
               "tryClamp succeeds when clamping OOB index");
    expectTrue(clampedOob == desc.maxFroxelIndex(), "tryClamp clamps OOB index to max");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::u32 emptyClamp = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampFroxelIndex(5u, zeroDesc, emptyClamp),
               "tryClamp rejects empty grid");
    expectTrue(emptyClamp == 0u, "tryClamp zeroes output on empty grid");
}

void testFroxelSampleCoordGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelSampleCoords inBounds{};
    inBounds.tileX0 = 1u;
    inBounds.tileY0 = 0u;
    inBounds.tileX1 = 2u;
    inBounds.tileY1 = 1u;
    inBounds.sliceZ0 = 1u;
    inBounds.sliceZ1 = 2u;
    inBounds.tx = 0.5f;
    inBounds.ty = 0.25f;
    inBounds.tz = 0.75f;
    expectTrue(fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(inBounds, desc),
               "in-bounds sample coords accepted");
    expectTrue(!fuse::renderer::FroxelGridLayout::isSampleCoordsOutOfRange(inBounds, desc),
               "in-bounds sample coords not out of range");

    fuse::renderer::FroxelSampleCoords oobTiles{};
    oobTiles.tileX0 = 99u;
    oobTiles.tileY0 = 0u;
    oobTiles.tileX1 = 1u;
    oobTiles.tileY1 = 1u;
    oobTiles.sliceZ0 = 0u;
    oobTiles.sliceZ1 = 1u;
    oobTiles.tx = 0.5f;
    oobTiles.ty = 0.5f;
    oobTiles.tz = 0.5f;
    expectTrue(!fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(oobTiles, desc),
               "OOB tile coords rejected");
    expectTrue(fuse::renderer::FroxelGridLayout::isSampleCoordsOutOfRange(oobTiles, desc),
               "OOB tile coords flagged out of range");

    fuse::renderer::FroxelSampleCoords oobWeights{};
    oobWeights.tileX0 = 0u;
    oobWeights.tileY0 = 0u;
    oobWeights.tileX1 = 1u;
    oobWeights.tileY1 = 1u;
    oobWeights.sliceZ0 = 0u;
    oobWeights.sliceZ1 = 1u;
    oobWeights.tx = 2.f;
    oobWeights.ty = -1.f;
    oobWeights.tz = 3.f;
    expectTrue(fuse::renderer::FroxelGridLayout::isSampleCoordsOutOfRange(oobWeights, desc),
               "OOB interpolation weights flagged out of range");

    fuse::renderer::FroxelSampleCoords clamped = oobWeights;
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(clamped, desc),
               "tryClamp sample coords succeeds on non-empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(clamped, desc),
               "tryClamp sample coords produces in-bounds coords");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::FroxelSampleCoords emptyClamp = oobWeights;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(emptyClamp, zeroDesc),
               "tryClamp sample coords rejects empty grid");
    expectTrue(!fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(inBounds, zeroDesc),
               "in-bounds coords rejected on empty grid desc");
}

void testFroxelDensityAccessAndValidationDeepen() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.5f;
    grid.density[23] = 2.5f;

    expectTrue(fuse::renderer::froxel_util::canSampleDensityAtIndex(grid, desc, 0u),
               "canSample accepts matching grid and desc");
    expectTrue(fuse::renderer::froxel_util::canSampleDensityAtIndex(grid, desc, 999u),
               "canSample accepts OOB index that will be clamped");

    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled),
               "trySample succeeds on valid grid");
    expectNear(sampled, 1.5f, 1e-5f, "trySample returns origin density");

    fuse::f32 clampedSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 999u, clampedSample),
               "trySample succeeds when clamping OOB index");
    expectNear(clampedSample, 2.5f, 1e-5f, "trySample clamps OOB index to last froxel");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    fuse::f32 rejectedSample = 9.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedSample),
               "trySample rejects empty storage");
    expectNear(rejectedSample, 0.f, 1e-6f, "trySample zeroes output on guard failure");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::canSampleDensityAtIndex(grid, mismatched, 0u),
               "canSample rejects desc mismatch");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, mismatched, 0u, 9.f),
               "tryWrite rejects desc mismatch");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 3.25f),
               "tryWrite succeeds on valid grid");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtIndex(grid, desc, 5u), 3.25f, 1e-5f,
               "tryWrite value readable via sample");

    fuse::renderer::FroxelDensityRejectReason reason = fuse::renderer::FroxelDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensity(grid, desc, reason),
               "tryValidate accepts allocated grid");
    expectTrue(reason == fuse::renderer::FroxelDensityRejectReason::None, "valid grid reports no reject reason");
    expectTrue(fuse::renderer::froxel_util::validateGridDensityForDesc(grid, desc),
               "validateGridDensityForDesc accepts matching desc");

    fuse::renderer::FroxelDensityGrid emptyStorage{};
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(emptyStorage, desc, reason),
               "tryValidate rejects empty storage");
    expectTrue(reason == fuse::renderer::FroxelDensityRejectReason::EmptyStorage,
               "empty storage reports empty_storage reason");
    expectTrue(std::strcmp(fuse::renderer::froxelDensityRejectReasonLabel(reason), "empty_storage") == 0,
               "reject reason label for empty storage");

    fuse::renderer::FroxelDensityGrid undersized{};
    undersized.density.resize(desc.froxelCount() - 1u, 0.f);
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(undersized, desc, reason),
               "tryValidate rejects undersized storage");
    expectTrue(reason == fuse::renderer::FroxelDensityRejectReason::DescMismatch,
               "undersized storage reports desc_mismatch reason");

    fuse::renderer::FroxelDensityGrid oversized{};
    oversized.density.resize(desc.froxelCount() + 1u, 0.f);
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(oversized, desc, reason),
               "tryValidate rejects oversized storage");
    expectTrue(reason == fuse::renderer::FroxelDensityRejectReason::DescMismatch,
               "oversized storage reports desc_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::froxelDensityRejectReasonLabel(reason), "desc_mismatch") == 0,
               "reject reason label for desc mismatch");

    expectTrue(!fuse::renderer::froxel_util::validateGridDensityForDesc(grid, mismatched),
               "validateGridDensityForDesc rejects desc mismatch");
}

void testZeroDimensionFroxelGrid() {
    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    zeroDesc.tilesY = 0u;
    zeroDesc.slicesZ = 0u;
    expectTrue(zeroDesc.isEmpty(), "zero tiles yields empty froxel desc");
    expectTrue(zeroDesc.froxelCount() == 0u, "zero-dimension froxel count is zero");

    fuse::u32 tileX = 99u;
    fuse::u32 tileY = 99u;
    fuse::u32 sliceZ = 99u;
    fuse::renderer::FroxelGridLayout::decodeFroxelIndex(5u, zeroDesc, tileX, tileY, sliceZ);
    expectTrue(tileX == 0u && tileY == 0u && sliceZ == 0u, "decode on empty grid returns origin");

    expectTrue(fuse::renderer::FroxelGridLayout::clampFroxelIndex(99u, zeroDesc) == 0u,
               "clamp froxel index on empty grid returns zero");
    expectTrue(fuse::renderer::FroxelGridLayout::clampTileX(99u, zeroDesc) == 0u, "clamp tile X on empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::clampTileY(99u, zeroDesc) == 0u, "clamp tile Y on empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::clampSliceZ(99u, zeroDesc) == 0u, "clamp slice Z on empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::froxelIndexClamped(99u, 99u, 99u, zeroDesc) == 0u,
               "clamped index on empty grid returns zero");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::u32 froxelIndex = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::mapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, froxelIndex),
               "screen mapping rejects empty froxel grid");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(zeroDesc);
    expectTrue(grid.isEmpty(), "allocate on zero-dimension desc stays empty");
    expectTrue(grid.matchesDesc(zeroDesc), "empty grid matches zero desc");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(grid) == 0u,
               "non-zero count zero for empty grid");
    expectTrue(fuse::renderer::froxel_util::countEmptyFroxels(grid) == 0u,
               "empty count zero when grid has no storage");
}

void testLightShaftsOcclusion() {
    fuse::renderer::LightShaftsParams params{};
    params.enabled = true;
    params.intensity = 2.f;

    const fuse::f32 blocked =
        fuse::renderer::evaluate_light_shaft_occlusion(0.8f, 0.5f, params.edge_softness);
    const fuse::f32 visible =
        fuse::renderer::evaluate_light_shaft_occlusion(0.5f, 0.55f, params.edge_softness);

    expectTrue(blocked == 0.f, "shaft behind scene depth is fully blocked");
    expectTrue(visible > 0.f, "shaft in front of scene depth contributes");

    fuse::renderer::LightShaftsPassStats stats{};
    expectTrue(fuse::renderer::record_light_shafts_pass(params, stats), "shafts pass records");
    expectTrue(stats.lastOcclusion > 0.f, "shafts pass tracks occlusion");
}

void testLensFlareGeneration() {
    fuse::renderer::LensFlareParams params{};
    params.ghost_count = 4;
    params.enabled = true;

    std::vector<fuse::renderer::LensFlareSample> elements;
    fuse::renderer::generate_lens_flare({0.5f, 0.5f},
                                        1.f,
                                        {1.f, 0.9f, 0.8f},
                                        1.f,
                                        params,
                                        elements);

    expectTrue(elements.size() >= params.ghost_count + 2u, "flare emits ghosts plus halo/starburst");
    expectTrue(elements.front().intensity > 0.f, "flare elements carry intensity");

    fuse::renderer::LensFlarePassStats stats{};
    expectTrue(fuse::renderer::record_lens_flare_pass(params,
                                                      {0.5f, 0.5f},
                                                      1.f,
                                                      {1.f, 1.f, 1.f},
                                                      1.f,
                                                      stats),
               "lens flare pass records");
    expectTrue(stats.lastElementCount == static_cast<fuse::u32>(elements.size()),
               "flare pass tracks element count");

    std::vector<fuse::renderer::LensFlareSample> occluded;
    fuse::renderer::generate_lens_flare({0.f, 0.f}, 1.f, {1.f, 1.f, 1.f}, 0.f, params, occluded);
    expectTrue(occluded.empty(), "fully occluded sun produces no flare elements");
}

void testDeferredPipelinePassHooks() {
    expectTrue(fuse::renderer::DeferredFramePipeline::passCount() == 19u,
               "deferred pipeline exposes 19 passes after B5.11 hooks");
    expectTrue(std::string(fuse::renderer::DeferredFramePipeline::passName(
                   fuse::renderer::DeferredPassId::VolumetricFog)) == "volumetric_fog",
               "volumetric fog pass name");
    expectTrue(std::string(fuse::renderer::DeferredFramePipeline::passName(
                   fuse::renderer::DeferredPassId::LightShafts)) == "light_shafts",
               "light shafts pass name");
    expectTrue(std::string(fuse::renderer::DeferredFramePipeline::passName(
                   fuse::renderer::DeferredPassId::LensFlare)) == "lens_flare",
               "lens flare pass name");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for B5.11 graph test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    auto renderer = fuse::renderer::DeferredRenderer::create({});
    renderer->init(resources);

    fuse::renderer::RenderGraph graph;
    expectTrue(renderer->buildFrameGraph(graph, 0u), "B5.11 frame graph compiled");
    expectTrue(graph.compileInfo().passCount == fuse::renderer::DeferredFramePipeline::passCount(),
               "B5.11 hook passes registered in deferred graph");
    expectTrue(renderer->pipeline().lastStats().cudaPassCount == 7u, "seven CUDA passes scheduled");
    expectTrue(renderer->pipeline().lastStats().vulkanPassCount == 12u, "twelve Vulkan passes scheduled");

    renderer->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testVolumetricFogDensity();
    testFroxelGridIndexing();
    testFroxelGridClampAndCountLimits();
    testFroxelSliceDepthDistribution();
    testFroxelDensityLerpHelpers();
    testFroxelIndexClampAndLerpGuards();
    testFroxelGridDensityValidationAndCoordGuards();
    testFroxelDensityCountValidationAndWriteGuards();
    testFroxelMaxIndexDeepenHelpers();
    testFroxelSampleCoordGuards();
    testFroxelDensityAccessAndValidationDeepen();
    testEmptySceneVolumetricFog();
    testZeroDimensionFroxelGrid();
    testFroxelPopulateFromAnalyticFog();
    testLightShaftsOcclusion();
    testLensFlareGeneration();
    testDeferredPipelinePassHooks();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_volumetric_lighting: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_volumetric_lighting: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
