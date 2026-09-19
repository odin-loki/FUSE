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

void testFroxelDensityAccessAndClampGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(desc.maxFroxelIndex() == 23u, "desc max froxel index matches last cell");
    expectTrue(fuse::renderer::FroxelGridLayout::maxFroxelIndex(desc) == desc.maxFroxelIndex(),
               "layout max froxel index delegates to desc");
    expectTrue(fuse::renderer::FroxelGridLayout::isAtMaxFroxelIndex(23u, desc),
               "last index is at max froxel index");
    expectTrue(!fuse::renderer::FroxelGridLayout::isAtMaxFroxelIndex(22u, desc),
               "non-last index is not at max froxel index");

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
    expectTrue(!fuse::renderer::FroxelGridLayout::isAtMaxFroxelIndex(0u, zeroDesc),
               "empty grid has no max froxel index");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    expectTrue(fuse::renderer::froxel_util::canLookupAtIndex(grid, desc, 0u),
               "allocated grid passes lookup preflight");
    expectTrue(fuse::renderer::froxel_util::canLookupAtIndex(grid, desc, 999u),
               "lookup preflight ignores index when storage matches desc");

    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 2.25f),
               "tryWrite at index succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 5u, sampled),
               "trySample at index succeeds on accessible grid");
    expectNear(sampled, 2.25f, 1e-5f, "trySample returns written density");

    fuse::f32 coordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 3.75f),
               "tryWrite at coord succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 1u, 1u, 2u, coordSample),
               "trySample at coord succeeds on accessible grid");
    expectNear(coordSample, 3.75f, 1e-5f, "trySample at coord returns written density");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    fuse::f32 rejectedSample = 1.f;
    expectTrue(!fuse::renderer::froxel_util::canLookupAtIndex(emptyGrid, desc, 0u),
               "empty storage fails lookup preflight");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedSample),
               "trySample rejects empty storage");
    expectNear(rejectedSample, 0.f, 1e-6f, "trySample zeroes output on rejection");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(emptyGrid, desc, 0u, 1.f),
               "tryWrite rejects empty storage");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtCoord(emptyGrid, desc, 0u, 0u, 0u, rejectedSample),
               "trySample at coord rejects empty storage");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtCoord(emptyGrid, desc, 0u, 0u, 0u, 1.f),
               "tryWrite at coord rejects empty storage");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::canLookupAtIndex(grid, mismatched, 0u),
               "desc mismatch fails lookup preflight");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, mismatched, 0u, 9.f),
               "tryWrite rejects desc mismatch");

    fuse::renderer::GridDensityRejectReason reason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensity(grid, desc, reason),
               "accessible grid validates density");
    expectTrue(reason == fuse::renderer::GridDensityRejectReason::None, "valid grid reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(reason), "none") == 0,
               "none reject reason label");

    fuse::renderer::FroxelDensityGrid undersized{};
    undersized.density.resize(desc.froxelCount() - 1u, 0.f);
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(undersized, desc, reason),
               "undersized storage fails density validation");
    expectTrue(reason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "undersized storage reports undersized reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(reason), "undersized_storage") == 0,
               "undersized reject reason label");

    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensity(emptyGrid, zeroDesc, reason),
               "empty desc vacuously validates density");
    expectTrue(reason == fuse::renderer::GridDensityRejectReason::None,
               "empty desc validation reports no reject reason");
}

void testFroxelDensityLookupAndSampleCoordGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);

    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelGrid(desc),
               "non-empty froxel desc does not skip grid ops");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelLookup(grid, desc),
               "allocated grid does not skip lookup");
    expectTrue(fuse::renderer::froxel_util::canLookupAtIndex(grid, desc, 0u),
               "accessible grid passes lookup preflight");

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 5u, lookupReason),
               "tryCanLookup succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "accessible grid reports no lookup reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "none") == 0,
               "none lookup reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelGrid(zeroDesc),
               "empty froxel desc skips grid ops");
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, zeroDesc, 0u, lookupReason),
               "empty froxel desc fails lookup preflight");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "empty_grid") == 0,
               "empty_grid lookup reject reason label");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(emptyGrid, desc, 0u, lookupReason),
               "empty storage fails lookup preflight");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage reject reason");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelLookup(emptyGrid, desc),
               "empty storage skips lookup");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, mismatched, 0u, lookupReason),
               "desc mismatch fails lookup preflight");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "desc_mismatch") == 0,
               "desc_mismatch lookup reject reason label");

    expectTrue(fuse::renderer::froxel_util::validateGridDensityForDesc(grid, desc),
               "validateGridDensityForDesc succeeds on accessible grid");

    fuse::renderer::FroxelSampleCoords inBounds{};
    inBounds.tileX0 = 1u;
    inBounds.tileY0 = 0u;
    inBounds.tileX1 = 2u;
    inBounds.tileY1 = 1u;
    inBounds.sliceZ0 = 1u;
    inBounds.sliceZ1 = 2u;
    inBounds.tx = 0.25f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.75f;
    expectTrue(fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(inBounds, desc),
               "in-bounds sample coords pass bounds check");
    expectTrue(!fuse::renderer::FroxelGridLayout::isSampleCoordsOutOfRange(inBounds, desc),
               "in-bounds sample coords are not out of range");

    fuse::renderer::FroxelSampleCoords outOfRange = inBounds;
    outOfRange.tileX0 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(outOfRange, desc),
               "OOB tile coord fails bounds check");
    expectTrue(fuse::renderer::FroxelGridLayout::isSampleCoordsOutOfRange(outOfRange, desc),
               "OOB tile coord is out of range");

    fuse::renderer::FroxelSampleCoords extremeWeights = inBounds;
    extremeWeights.tx = 2.f;
    expectTrue(fuse::renderer::FroxelGridLayout::isSampleCoordsOutOfRange(extremeWeights, desc),
               "OOB interpolation weight is out of range");

    fuse::renderer::FroxelSampleCoords unclamped = outOfRange;
    const fuse::u32 originalTileX = unclamped.tileX0;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(unclamped, zeroDesc),
               "tryClampSampleCoords rejects empty grid");
    expectTrue(unclamped.tileX0 == originalTileX, "tryClampSampleCoords leaves coords unchanged on empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(unclamped, desc),
               "tryClampSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(unclamped, desc),
               "tryClampSampleCoords produces in-bounds coords");

    expectTrue(fuse::renderer::froxel_util::writeDensityAtIndex(grid, desc, 0u, 1.f),
               "seed origin density for guarded sample tests");

    fuse::f32 bilinearSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, bilinearSample),
               "trySampleDensityBilinear succeeds on accessible grid");
    expectNear(bilinearSample,
               fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, inBounds),
               1e-5f,
               "trySampleDensityBilinear matches unguarded sample");

    fuse::f32 trilinearSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, inBounds, trilinearSample),
               "trySampleDensityTrilinear succeeds on accessible grid");
    expectNear(trilinearSample,
               fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, inBounds),
               1e-5f,
               "trySampleDensityTrilinear matches unguarded sample");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::f32 screenSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample),
               "trySampleDensityAtScreen succeeds on accessible grid");
    expectNear(screenSample,
               fuse::renderer::froxel_util::sampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               1e-5f,
               "trySampleDensityAtScreen matches unguarded screen sample");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   emptyGrid, desc, camera, 0.5f, 0.5f, 10.f, rejectedScreen),
               "trySampleDensityAtScreen rejects empty storage");
    expectNear(rejectedScreen, 0.f, 1e-6f, "trySampleDensityAtScreen zeroes output on rejection");

    fuse::f32 rejectedTrilinear = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(emptyGrid, desc, inBounds, rejectedTrilinear),
               "trySampleDensityTrilinear rejects empty storage");
    expectNear(rejectedTrilinear, 0.f, 1e-6f, "trySampleDensityTrilinear zeroes output on rejection");
}

void testFroxelSampleCoordNormalizeAndScreenMappingGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelSampleCoords reversed{};
    reversed.tileX0 = 2u;
    reversed.tileY0 = 1u;
    reversed.tileX1 = 1u;
    reversed.tileY1 = 0u;
    reversed.sliceZ0 = 2u;
    reversed.sliceZ1 = 1u;
    reversed.tx = 0.25f;
    reversed.ty = 0.75f;
    reversed.tz = 0.5f;
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidSampleCoords(reversed, desc),
               "reversed sample corners fail validity check");
    fuse::renderer::FroxelGridLayout::normalizeSampleCoords(reversed);
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(reversed, desc),
               "normalizeSampleCoords fixes reversed corners");

    fuse::renderer::FroxelSampleCoords extremeWeights{};
    extremeWeights.tileX0 = 0u;
    extremeWeights.tileY0 = 0u;
    extremeWeights.tileX1 = 1u;
    extremeWeights.tileY1 = 1u;
    extremeWeights.sliceZ0 = 0u;
    extremeWeights.sliceZ1 = 1u;
    extremeWeights.tx = 2.f;
    extremeWeights.ty = -1.f;
    extremeWeights.tz = 3.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidSampleCoords(extremeWeights, desc),
               "OOB interpolation weights fail validity check");
    fuse::renderer::FroxelGridLayout::normalizeSampleCoords(extremeWeights);
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(extremeWeights, desc),
               "normalizeSampleCoords clamps interpolation weights");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::ScreenMappingRejectReason mapReason = fuse::renderer::ScreenMappingRejectReason::None;
    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, desc, camera, mapped, mapReason),
               "tryMapScreenDepthToSampleCoords succeeds in range");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::None,
               "successful screen mapping reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::screenMappingRejectReasonLabel(mapReason), "none") == 0,
               "none screen-mapping reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, mapped, mapReason),
               "tryMapScreenDepthToSampleCoords rejects empty grid");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "empty grid reports empty_grid screen-mapping reject reason");
    expectTrue(std::strcmp(fuse::renderer::screenMappingRejectReasonLabel(mapReason), "empty_grid") == 0,
               "empty_grid screen-mapping reject reason label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, mapped, mapReason),
               "tryMapScreenDepthToSampleCoords rejects depth below near plane");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range reject reason");

    fuse::renderer::FroxelCameraDesc badCamera{};
    badCamera.nearPlane = 100.f;
    badCamera.farPlane = 1.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 50.f, desc, badCamera, mapped, mapReason),
               "tryMapScreenDepthToSampleCoords rejects invalid camera");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera reject reason");

    fuse::u32 froxelIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, desc, camera, froxelIndex, mapReason),
               "tryMapScreenDepthToFroxelIndex succeeds in range");
    expectTrue(froxelIndex < desc.froxelCount(), "mapped froxel index in bounds");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;

    fuse::renderer::FroxelSampleCoords inBounds{};
    inBounds.tileX0 = 0u;
    inBounds.tileY0 = 0u;
    inBounds.tileX1 = 1u;
    inBounds.tileY1 = 1u;
    inBounds.sliceZ0 = 0u;
    inBounds.sliceZ1 = 1u;
    inBounds.tx = 0.5f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.5f;

    fuse::renderer::SampleCoordRejectReason sampleReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, inBounds, sampleReason),
               "tryCanSampleAtCoords succeeds on accessible grid");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::None,
               "in-bounds coords report no sample reject reason");

    fuse::renderer::FroxelSampleCoords warnCoords = inBounds;
    warnCoords.tx = 2.f;
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, warnCoords, sampleReason),
               "tryCanSampleAtCoords still succeeds when weights will be clamped");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "OOB weights report invalid_weights sample reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(sampleReason), "invalid_weights") == 0,
               "invalid_weights sample reject reason label");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(emptyGrid, desc, inBounds, sampleReason),
               "tryCanSampleAtCoords rejects empty storage");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "empty storage maps to out_of_bounds sample reject reason");

    fuse::f32 bilinearSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, bilinearSample, sampleReason),
               "trySampleDensityBilinear with reason succeeds on accessible grid");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::None,
               "successful bilinear sample reports no reject reason");

    fuse::renderer::FroxelDensityGrid oversized{};
    oversized.density.resize(desc.froxelCount() + 1u, 0.f);
    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(oversized, desc, densityReason),
               "oversized storage fails grid density validation");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
               "oversized storage reports desc_mismatch reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "desc_mismatch") == 0,
               "desc_mismatch grid density reject reason label");
}

void testFroxelPopulatePreflightAndLookupGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 2;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.march_steps = 32u;

    expectTrue(fuse::renderer::froxel_util::canPopulateFromAnalyticFog(desc, camera, params),
               "valid populate preflight succeeds");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, camera, params),
               "valid populate does not skip");

    fuse::renderer::FroxelPopulateRejectReason populateReason = fuse::renderer::FroxelPopulateRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, params, populateReason),
               "tryCanPopulate succeeds for valid inputs");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::None,
               "valid populate reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "none") == 0,
               "none populate reject reason label");

    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, camera, zeroDensity),
               "zero density skips populate fill");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroDensity, populateReason),
               "zero density fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroDensity,
               "zero density reports zero_density reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "zero_density") == 0,
               "zero_density populate reject reason label");

    fuse::renderer::VolumetricFogParams zeroMarch = params;
    zeroMarch.march_steps = 0u;
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroMarch, populateReason),
               "zero march steps fail populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroMarchSteps,
               "zero march steps report zero_march_steps reject reason");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroDesc, camera, params),
               "empty desc skips populate fill");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(zeroDesc, camera, params, populateReason),
               "empty desc fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::EmptyDesc,
               "empty desc reports empty_desc populate reject reason");

    fuse::renderer::FroxelCameraDesc badCamera{};
    badCamera.nearPlane = 100.f;
    badCamera.farPlane = 1.f;
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, badCamera, params, populateReason),
               "invalid camera fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera populate reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "invalid_camera") == 0,
               "invalid_camera populate reject reason label");

    fuse::renderer::FroxelDensityGrid guardedGrid{};
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(guardedGrid, desc, camera, params),
               "tryPopulate succeeds for valid inputs");
    expectTrue(guardedGrid.matchesDesc(desc), "tryPopulate allocates matching grid");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(guardedGrid) == desc.froxelCount(),
               "tryPopulate fills all froxels on success");

    fuse::renderer::FroxelDensityGrid skippedGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(skippedGrid, desc, camera, zeroDensity),
               "tryPopulate returns false when preflight rejects fill");
    expectTrue(skippedGrid.matchesDesc(desc), "tryPopulate still allocates on rejected fill");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(skippedGrid) == 0u,
               "tryPopulate leaves zero density when preflight rejects fill");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;
    grid.density[desc.maxFroxelIndex()] = 2.f;

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 0u, lookupReason),
               "origin index lookup preflight succeeds");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "in-range index reports no lookup reject reason");

    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 999u, lookupReason),
               "OOB index lookup preflight still succeeds with clamp warning");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "OOB index reports index_out_of_range lookup reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "index_out_of_range") == 0,
               "index_out_of_range lookup reject reason label");
    expectTrue(fuse::renderer::froxel_util::wouldClampDensityLookupIndex(999u, desc),
               "wouldClampDensityLookupIndex true for OOB index");
    expectTrue(!fuse::renderer::froxel_util::wouldClampDensityLookupIndex(desc.maxFroxelIndex(), desc),
               "wouldClampDensityLookupIndex false for last valid index");

    fuse::renderer::FroxelSampleCoords inBounds{};
    inBounds.tileX0 = 0u;
    inBounds.tileY0 = 0u;
    inBounds.tileX1 = 1u;
    inBounds.tileY1 = 1u;
    inBounds.sliceZ0 = 0u;
    inBounds.sliceZ1 = 1u;
    inBounds.tx = 0.5f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.5f;
    expectTrue(fuse::renderer::FroxelGridLayout::canPreflightSampleCoords(inBounds, desc),
               "canPreflightSampleCoords succeeds for in-bounds coords");
    fuse::renderer::SampleCoordRejectReason sampleReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(inBounds, desc, sampleReason),
               "tryPreflightSampleCoords succeeds for in-bounds coords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::None,
               "in-bounds coords report no sample preflight reject reason");
    expectTrue(!fuse::renderer::FroxelGridLayout::wouldClampSampleCoords(inBounds, desc),
               "in-bounds coords would not clamp");

    fuse::renderer::FroxelSampleCoords warnWeights = inBounds;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(warnWeights, desc, sampleReason),
               "tryPreflightSampleCoords warns but succeeds for clampable weights");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "clampable weights report invalid_weights sample preflight reason");
    expectTrue(fuse::renderer::FroxelGridLayout::wouldClampSampleCoords(warnWeights, desc),
               "clampable weights would clamp before sampling");

    fuse::renderer::FroxelSampleCoords hardOob = inBounds;
    hardOob.tileX0 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(hardOob, desc, sampleReason),
               "tryPreflightSampleCoords rejects hard OOB tile coord");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "hard OOB tile coord reports out_of_bounds sample preflight reason");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, hardOob, sampleReason),
               "tryCanSampleAtCoords rejects hard OOB tile coord");
    expectTrue(fuse::renderer::FroxelGridLayout::wouldClampSampleCoords(hardOob, desc),
               "hard OOB coords would clamp before sampling");

    fuse::renderer::FroxelGridDesc emptyDesc{};
    emptyDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(inBounds, emptyDesc, sampleReason),
               "tryPreflightSampleCoords rejects empty desc");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty desc reports empty_grid sample preflight reason");

    fuse::f32 guardedBilinear = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, guardedBilinear, sampleReason),
               "trySampleDensityBilinear with reason succeeds for in-bounds coords");
    expectNear(guardedBilinear,
               fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, inBounds),
               1e-5f,
               "trySampleDensityBilinear with reason matches unguarded sample");

    fuse::f32 rejectedBilinear = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, hardOob, rejectedBilinear, sampleReason),
               "trySampleDensityBilinear rejects hard OOB coords");
    expectNear(rejectedBilinear, 0.f, 1e-6f, "trySampleDensityBilinear zeroes output on hard OOB rejection");
    expectTrue(fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, hardOob) >= 0.f,
               "unguarded bilinear sample still clamps hard OOB coords");
}

void testFroxelTrilinearAndWouldSkipGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;
    grid.density[desc.maxFroxelIndex()] = 2.f;

    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookup(grid, desc),
               "accessible grid does not skip density lookup");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookup(grid, desc, 0u),
               "in-range index does not skip density lookup");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookup(grid, desc, 999u),
               "OOB index that clamps does not skip density lookup");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookupAtCoord(grid, desc, 1u, 1u, 2u),
               "in-range coords do not skip density lookup");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookupAtCoord(grid, desc, 99u, 99u, 99u),
               "OOB coords that clamp do not skip density lookup");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookup(emptyGrid, desc),
               "empty storage skips density lookup");
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookup(emptyGrid, desc, 0u),
               "empty storage skips index density lookup");
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u),
               "empty storage skips coord density lookup");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookup(grid, zeroDesc),
               "empty froxel desc skips density lookup");

    fuse::renderer::FroxelSampleCoords inBounds{};
    inBounds.tileX0 = 0u;
    inBounds.tileY0 = 0u;
    inBounds.tileX1 = 1u;
    inBounds.tileY1 = 1u;
    inBounds.sliceZ0 = 0u;
    inBounds.sliceZ1 = 1u;
    inBounds.tx = 0.5f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.5f;

    expectTrue(fuse::renderer::froxel_util::canTrilinearSampleAtCoords(grid, desc, inBounds),
               "canTrilinearSampleAtCoords succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityTrilinearSample(grid, desc, inBounds),
               "in-bounds coords do not skip trilinear sample");

    fuse::renderer::FroxelTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::FroxelTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanTrilinearSampleAtCoords(grid, desc, inBounds, trilinearReason),
               "tryCanTrilinearSampleAtCoords succeeds on accessible grid");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::None,
               "in-bounds coords report no trilinear reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason), "none") == 0,
               "none trilinear reject reason label");

    fuse::renderer::FroxelSampleCoords warnWeights = inBounds;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::froxel_util::tryCanTrilinearSampleAtCoords(grid, desc, warnWeights, trilinearReason),
               "tryCanTrilinearSampleAtCoords warns but succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::ClampableWeights,
               "clampable weights report clampable_weights trilinear reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason),
                           "clampable_weights") == 0,
               "clampable_weights trilinear reject reason label");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityTrilinearSample(grid, desc, warnWeights),
               "clampable weights do not skip trilinear sample");

    fuse::renderer::FroxelSampleCoords hardOob = inBounds;
    hardOob.tileX0 = 99u;
    expectTrue(!fuse::renderer::froxel_util::tryCanTrilinearSampleAtCoords(grid, desc, hardOob, trilinearReason),
               "tryCanTrilinearSampleAtCoords rejects hard OOB tile coord");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::InvalidSampleCoords,
               "hard OOB tile coord reports invalid_sample_coords trilinear reject reason");
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityTrilinearSample(grid, desc, hardOob),
               "hard OOB coords skip trilinear sample");
    expectTrue(!fuse::renderer::FroxelGridLayout::wouldSkipSampleCoordPreflight(inBounds, desc),
               "in-bounds coords do not skip sample-coord preflight");
    expectTrue(fuse::renderer::FroxelGridLayout::wouldSkipSampleCoordPreflight(hardOob, desc),
               "hard OOB coords skip sample-coord preflight");

    expectTrue(!fuse::renderer::froxel_util::tryCanTrilinearSampleAtCoords(emptyGrid, desc, inBounds, trilinearReason),
               "tryCanTrilinearSampleAtCoords rejects empty storage");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::InaccessibleGrid,
               "empty storage reports inaccessible_grid trilinear reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason),
                           "inaccessible_grid") == 0,
               "inaccessible_grid trilinear reject reason label");

    expectTrue(!fuse::renderer::froxel_util::tryCanTrilinearSampleAtCoords(grid, zeroDesc, inBounds, trilinearReason),
               "tryCanTrilinearSampleAtCoords rejects empty froxel desc");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid trilinear reject reason");

    fuse::f32 trilinearSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, inBounds, trilinearSample, trilinearReason),
               "trySampleDensityTrilinear with trilinear reason succeeds on accessible grid");
    expectNear(trilinearSample,
               fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, inBounds),
               1e-5f,
               "trySampleDensityTrilinear with trilinear reason matches unguarded sample");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::None,
               "successful trilinear sample reports no trilinear reject reason");

    fuse::f32 rejectedTrilinear = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, hardOob, rejectedTrilinear, trilinearReason),
               "trySampleDensityTrilinear with trilinear reason rejects hard OOB coords");
    expectNear(rejectedTrilinear, 0.f, 1e-6f, "trySampleDensityTrilinear with trilinear reason zeroes output on rejection");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::InvalidSampleCoords,
               "rejected trilinear sample reports invalid_sample_coords reason");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.march_steps = 32u;

    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, params),
               "valid populate inputs do not skip froxel populate");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(zeroDesc, camera, params),
               "empty desc skips froxel populate");

    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroDensity),
               "zero density skips froxel populate");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroDensity) ==
                   fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, camera, zeroDensity),
               "wouldSkipFroxelPopulate agrees with shouldSkipFroxelPopulate for zero density");

    fuse::renderer::VolumetricFogParams zeroMarch = params;
    zeroMarch.march_steps = 0u;
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroMarch),
               "zero march steps skip froxel populate");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroMarch) ==
                   fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, camera, zeroMarch),
               "wouldSkipFroxelPopulate agrees with shouldSkipFroxelPopulate for zero march steps");

    fuse::renderer::FroxelCameraDesc badCamera{};
    badCamera.nearPlane = 100.f;
    badCamera.farPlane = 1.f;
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, badCamera, params),
               "invalid camera skips froxel populate");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, badCamera, params),
               "shouldSkipFroxelPopulate ignores invalid camera");
}

void testFroxelCoordLookupAndDiagnosticGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(!fuse::renderer::FroxelGridLayout::isCoordOutOfRange(0u, 0u, 0u, desc),
               "origin coords are in range");
    expectTrue(!fuse::renderer::FroxelGridLayout::isCoordOutOfRange(3u, 1u, 2u, desc),
               "last tile/slice coords are in range");
    expectTrue(fuse::renderer::FroxelGridLayout::isCoordOutOfRange(99u, 1u, 2u, desc),
               "OOB tile X is out of range");
    expectTrue(fuse::renderer::FroxelGridLayout::isCoordOutOfRange(0u, 99u, 2u, desc),
               "OOB tile Y is out of range");
    expectTrue(fuse::renderer::FroxelGridLayout::isCoordOutOfRange(0u, 0u, 99u, desc),
               "OOB slice Z is out of range");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::isCoordOutOfRange(0u, 0u, 0u, zeroDesc),
               "any coord is out of range on empty grid");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;
    grid.density[desc.maxFroxelIndex()] = 2.f;

    expectTrue(fuse::renderer::froxel_util::canLookupAtCoord(grid, desc, 1u, 1u, 2u),
               "accessible grid passes coord lookup preflight");
    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, desc, 1u, 1u, 2u, lookupReason),
               "tryCanLookupAtCoord succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "in-range coords report no lookup reject reason");

    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, desc, 99u, 99u, 99u, lookupReason),
               "OOB coord lookup preflight still succeeds with clamp warning");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "OOB coords report index_out_of_range lookup reject reason");
    expectTrue(fuse::renderer::froxel_util::wouldClampDensityLookupCoord(99u, 99u, 99u, desc),
               "wouldClampDensityLookupCoord true for OOB coords");
    expectTrue(!fuse::renderer::froxel_util::wouldClampDensityLookupCoord(3u, 1u, 2u, desc),
               "wouldClampDensityLookupCoord false for last valid coords");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u, lookupReason),
               "tryCanLookupAtCoord rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage lookup reject reason");

    fuse::f32 coordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordSample, lookupReason),
               "trySampleDensityAtCoord with reason succeeds on accessible grid");
    expectNear(coordSample, 1.f, 1e-5f, "trySampleDensityAtCoord with reason returns origin density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful coord sample reports no lookup reject reason");

    fuse::f32 oobCoordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 99u, 99u, 99u, oobCoordSample, lookupReason),
               "trySampleDensityAtCoord with reason clamps OOB coords");
    expectNear(oobCoordSample, 2.f, 1e-5f, "trySampleDensityAtCoord with reason clamps to last cell");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "OOB coord sample reports index_out_of_range lookup reject reason");

    fuse::f32 indexSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 999u, indexSample, lookupReason),
               "trySampleDensityAtIndex with reason clamps OOB index");
    expectNear(indexSample, 2.f, 1e-5f, "trySampleDensityAtIndex with reason returns clamped density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "OOB index sample reports index_out_of_range lookup reject reason");

    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 3.75f, lookupReason),
               "tryWriteDensityAtCoord with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful coord write reports no lookup reject reason");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 4.25f, lookupReason),
               "tryWriteDensityAtIndex with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index write reports no lookup reject reason");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(emptyGrid, desc, 0u, 1.f, lookupReason),
               "tryWriteDensityAtIndex with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "rejected index write reports empty_storage lookup reject reason");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::f32 screenSample = 0.f;
    fuse::renderer::ScreenMappingRejectReason mapReason = fuse::renderer::ScreenMappingRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, mapReason),
               "trySampleDensityAtScreen with reason succeeds on accessible grid");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::None,
               "successful screen sample reports no screen-mapping reject reason");
    expectNear(screenSample,
               fuse::renderer::froxel_util::sampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               1e-5f,
               "trySampleDensityAtScreen with reason matches unguarded sample");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   emptyGrid, desc, camera, 0.5f, 0.5f, 10.f, rejectedScreen, mapReason),
               "trySampleDensityAtScreen with reason rejects empty storage");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "empty storage maps to empty_grid screen-mapping reject reason");
    expectNear(rejectedScreen, 0.f, 1e-6f, "trySampleDensityAtScreen with reason zeroes output on rejection");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, rejectedScreen, mapReason),
               "trySampleDensityAtScreen with reason rejects depth below near plane");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range screen-mapping reject reason");

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.march_steps = 32u;
    fuse::renderer::FroxelPopulateRejectReason populateReason = fuse::renderer::FroxelPopulateRejectReason::None;
    fuse::renderer::FroxelDensityGrid populated{};
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, desc, camera, params, populateReason),
               "tryPopulate with reason succeeds for valid inputs");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::None,
               "successful populate reports no reject reason");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(populated) == desc.froxelCount(),
               "tryPopulate with reason fills all froxels on success");

    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    fuse::renderer::FroxelDensityGrid skipped{};
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(skipped, desc, camera, zeroDensity, populateReason),
               "tryPopulate with reason returns false when preflight rejects fill");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroDensity,
               "rejected populate reports zero_density reject reason");
    expectTrue(skipped.matchesDesc(desc), "tryPopulate with reason still allocates on rejected fill");
}

void testFroxelClassifyPreflightAndIsBlockingGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.march_steps = 32u;

    expectTrue(!fuse::renderer::screenMappingRejectReasonIsBlocking(fuse::renderer::ScreenMappingRejectReason::None),
               "None screen-mapping reject reason is not blocking");
    expectTrue(fuse::renderer::screenMappingRejectReasonIsBlocking(fuse::renderer::ScreenMappingRejectReason::EmptyGrid),
               "empty_grid screen-mapping reject reason is blocking");
    expectTrue(!fuse::renderer::sampleCoordRejectReasonIsBlocking(fuse::renderer::SampleCoordRejectReason::None),
               "None sample-coord reject reason is not blocking");
    expectTrue(!fuse::renderer::sampleCoordRejectReasonIsBlocking(
                   fuse::renderer::SampleCoordRejectReason::InvalidWeights),
               "invalid_weights sample-coord reject reason is not blocking");
    expectTrue(fuse::renderer::sampleCoordRejectReasonIsBlocking(
                   fuse::renderer::SampleCoordRejectReason::OutOfBounds),
               "out_of_bounds sample-coord reject reason is blocking");
    expectTrue(!fuse::renderer::densityLookupRejectReasonIsBlocking(fuse::renderer::DensityLookupRejectReason::None),
               "None density lookup reject reason is not blocking");
    expectTrue(!fuse::renderer::densityLookupRejectReasonIsBlocking(
                   fuse::renderer::DensityLookupRejectReason::IndexOutOfRange),
               "index_out_of_range density lookup reject reason is not blocking");
    expectTrue(fuse::renderer::densityLookupRejectReasonIsBlocking(
                   fuse::renderer::DensityLookupRejectReason::EmptyStorage),
               "empty_storage density lookup reject reason is blocking");
    expectTrue(!fuse::renderer::froxelTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::FroxelTrilinearSampleRejectReason::ClampableWeights),
               "clampable_weights trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::froxelTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::FroxelTrilinearSampleRejectReason::InvalidSampleCoords),
               "invalid_sample_coords trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::gridDensityRejectReasonIsBlocking(fuse::renderer::GridDensityRejectReason::None),
               "None grid-density reject reason is not blocking");
    expectTrue(fuse::renderer::gridDensityRejectReasonIsBlocking(
                   fuse::renderer::GridDensityRejectReason::UndersizedStorage),
               "undersized_storage grid-density reject reason is blocking");
    expectTrue(!fuse::renderer::froxelPopulateRejectReasonIsBlocking(
                   fuse::renderer::FroxelPopulateRejectReason::None),
               "None populate reject reason is not blocking");
    expectTrue(fuse::renderer::froxelPopulateRejectReasonIsBlocking(
                   fuse::renderer::FroxelPopulateRejectReason::ZeroDensity),
               "zero_density populate reject reason is blocking");

    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMapping(
                   0.5f, 0.5f, 10.f, desc, camera, &mapped),
               "preflightScreenMapping succeeds in range");
    expectTrue(mapped.tileX0 < desc.tilesX && mapped.sliceZ0 < desc.slicesZ,
               "preflightScreenMapping returns mapped coords");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenMappingReject(0.5f, 0.5f, 10.f, desc, camera) ==
                   fuse::renderer::ScreenMappingRejectReason::None,
               "classifyScreenMappingReject none for valid mapping");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::ScreenMappingRejectReason mapReason = fuse::renderer::ScreenMappingRejectReason::None;
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenMapping(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, nullptr, &mapReason),
               "preflightScreenMapping rejects empty grid");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "preflightScreenMapping reports empty_grid reject reason");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenMappingReject(0.5f, 0.5f, 10.f, zeroDesc, camera) ==
                   fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "classifyScreenMappingReject empty_grid for empty desc");

    fuse::renderer::FroxelSampleCoords inBounds{};
    inBounds.tileX0 = 0u;
    inBounds.tileY0 = 0u;
    inBounds.tileX1 = 1u;
    inBounds.tileY1 = 1u;
    inBounds.sliceZ0 = 0u;
    inBounds.sliceZ1 = 1u;
    inBounds.tx = 0.5f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.5f;
    fuse::renderer::SampleCoordRejectReason sampleReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(inBounds, desc, &sampleReason),
               "preflightSampleCoords succeeds for in-bounds coords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::None,
               "preflightSampleCoords reports no reject reason for in-bounds coords");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordReject(inBounds, desc) ==
                   fuse::renderer::SampleCoordRejectReason::None,
               "classifySampleCoordReject none for in-bounds coords");

    fuse::renderer::FroxelSampleCoords warnWeights = inBounds;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(warnWeights, desc, &sampleReason),
               "preflightSampleCoords succeeds for clampable weights");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "preflightSampleCoords reports invalid_weights for clampable weights");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordReject(warnWeights, desc) ==
                   fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "classifySampleCoordReject invalid_weights for clampable weights");

    fuse::renderer::FroxelSampleCoords hardOob = inBounds;
    hardOob.tileX0 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightSampleCoords(hardOob, desc, &sampleReason),
               "preflightSampleCoords rejects hard OOB tile coord");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "preflightSampleCoords reports out_of_bounds for hard OOB tile coord");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 0u, &lookupReason),
               "preflightDensityLookup succeeds for accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "preflightDensityLookup reports no reject reason for in-range index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(grid, desc, 0u) ==
                   fuse::renderer::DensityLookupRejectReason::None,
               "classifyDensityLookupReject none for in-range index");

    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 999u, &lookupReason),
               "preflightDensityLookup succeeds for OOB index that clamps");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "preflightDensityLookup reports index_out_of_range for OOB index");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 99u, 99u, 99u, &lookupReason),
               "preflightDensityLookupAtCoord succeeds for OOB coords that clamp");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "preflightDensityLookupAtCoord reports index_out_of_range for OOB coords");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookup(emptyGrid, desc, 0u, &lookupReason),
               "preflightDensityLookup rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "preflightDensityLookup reports empty_storage for empty grid");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(emptyGrid, desc, 0u) ==
                   fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "classifyDensityLookupReject empty_storage for empty grid");

    fuse::renderer::FroxelTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::FroxelTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, inBounds, &trilinearReason),
               "preflightTrilinearSample succeeds for in-bounds coords");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::None,
               "preflightTrilinearSample reports no reject reason for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelTrilinearSampleReject(grid, desc, inBounds) ==
                   fuse::renderer::FroxelTrilinearSampleRejectReason::None,
               "classifyFroxelTrilinearSampleReject none for in-bounds coords");

    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, warnWeights, &trilinearReason),
               "preflightTrilinearSample succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::ClampableWeights,
               "preflightTrilinearSample reports clampable_weights for OOB weights");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, hardOob, &trilinearReason),
               "preflightTrilinearSample rejects hard OOB tile coord");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::InvalidSampleCoords,
               "preflightTrilinearSample reports invalid_sample_coords for hard OOB tile coord");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::preflightGridDensity(grid, desc, &densityReason),
               "preflightGridDensity succeeds for accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "preflightGridDensity reports no reject reason for accessible grid");
    expectTrue(fuse::renderer::froxel_util::classifyGridDensityReject(grid, desc) ==
                   fuse::renderer::GridDensityRejectReason::None,
               "classifyGridDensityReject none for accessible grid");

    fuse::renderer::FroxelDensityGrid undersized{};
    undersized.density.resize(desc.froxelCount() - 1u, 0.f);
    expectTrue(!fuse::renderer::froxel_util::preflightGridDensity(undersized, desc, &densityReason),
               "preflightGridDensity rejects undersized storage");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "preflightGridDensity reports undersized_storage reject reason");
    expectTrue(fuse::renderer::froxel_util::classifyGridDensityReject(undersized, desc) ==
                   fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "classifyGridDensityReject undersized_storage for undersized storage");

    fuse::renderer::FroxelPopulateRejectReason populateReason = fuse::renderer::FroxelPopulateRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::preflightFroxelPopulate(desc, camera, params, &populateReason),
               "preflightFroxelPopulate succeeds for valid inputs");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::None,
               "preflightFroxelPopulate reports no reject reason for valid inputs");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelPopulateReject(desc, camera, params) ==
                   fuse::renderer::FroxelPopulateRejectReason::None,
               "classifyFroxelPopulateReject none for valid inputs");

    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(desc, camera, zeroDensity, &populateReason),
               "preflightFroxelPopulate rejects zero density");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroDensity,
               "preflightFroxelPopulate reports zero_density reject reason");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelPopulateReject(desc, camera, zeroDensity) ==
                   fuse::renderer::FroxelPopulateRejectReason::ZeroDensity,
               "classifyFroxelPopulateReject zero_density for zero density");

    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(inBounds, desc) ==
                   fuse::renderer::FroxelGridLayout::canPreflightSampleCoords(inBounds, desc),
               "preflightSampleCoords agrees with canPreflightSampleCoords for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 0u) ==
                   fuse::renderer::froxel_util::canLookupAtIndex(grid, desc, 0u),
               "preflightDensityLookup agrees with canLookupAtIndex for accessible grid");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelPopulate(desc, camera, params) ==
                   fuse::renderer::froxel_util::canPopulateFromAnalyticFog(desc, camera, params),
               "preflightFroxelPopulate agrees with canPopulateFromAnalyticFog for valid inputs");
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
    testFroxelDensityAccessAndClampGuards();
    testFroxelDensityLookupAndSampleCoordGuards();
    testFroxelSampleCoordNormalizeAndScreenMappingGuards();
    testFroxelPopulatePreflightAndLookupGuards();
    testFroxelTrilinearAndWouldSkipGuards();
    testFroxelCoordLookupAndDiagnosticGuards();
    testFroxelClassifyPreflightAndIsBlockingGuards();
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

// --- deepen additive from deepen-b511-froxel-density-guards-7755 ---
void testFroxelValidateGridDensityAndAccessGuards() {

// --- deepen additive from deepen-froxel-density-guards-1bcd ---
void testFroxelDensityAccessGuardsAndValidation() {
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampledDensity),
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 3.5f),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 5u, writtenDensity),
    fuse::renderer::DensityGridRejectReason reason = fuse::renderer::DensityGridRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateDensityCounts(grid, desc, reason),
    expectTrue(reason == fuse::renderer::DensityGridRejectReason::None, "valid grid reports no reject reason");
    expectTrue(fuse::renderer::froxel_util::tryValidateDensityCounts(emptyGrid, zeroDesc, reason),
    expectTrue(reason == fuse::renderer::DensityGridRejectReason::None,
    expectTrue(!fuse::renderer::froxel_util::tryValidateDensityCounts(emptyGrid, desc, reason),
    expectTrue(reason == fuse::renderer::DensityGridRejectReason::EmptyGridDesc,
    expectTrue(std::string(fuse::renderer::densityGridRejectReasonLabel(reason)) == "empty_grid_desc",
    expectTrue(!fuse::renderer::froxel_util::tryValidateDensityCounts(grid, mismatched, reason),
    expectTrue(reason == fuse::renderer::DensityGridRejectReason::DescMismatch,
    expectTrue(std::string(fuse::renderer::densityGridRejectReasonLabel(reason)) == "desc_mismatch",

// --- deepen additive from deepen-froxel-density-guards-6d29 ---
void testFroxelSampleCoordGuards() {
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(clamped, desc),
               "tryClamp sample coords succeeds on non-empty grid");
               "tryClamp sample coords produces in-bounds coords");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(emptyClamp, zeroDesc),
               "tryClamp sample coords rejects empty grid");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled),
               "trySample succeeds on valid grid");
    expectNear(sampled, 1.5f, 1e-5f, "trySample returns origin density");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 999u, clampedSample),
               "trySample succeeds when clamping OOB index");
    expectNear(clampedSample, 2.5f, 1e-5f, "trySample clamps OOB index to last froxel");
    expectNear(rejectedSample, 0.f, 1e-6f, "trySample zeroes output on guard failure");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 3.25f),
               "tryWrite succeeds on valid grid");
               "tryWrite value readable via sample");
    fuse::renderer::FroxelDensityRejectReason reason = fuse::renderer::FroxelDensityRejectReason::None;
               "tryValidate accepts allocated grid");
    expectTrue(reason == fuse::renderer::FroxelDensityRejectReason::None, "valid grid reports no reject reason");
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(emptyStorage, desc, reason),
               "tryValidate rejects empty storage");
    expectTrue(reason == fuse::renderer::FroxelDensityRejectReason::EmptyStorage,
    expectTrue(std::strcmp(fuse::renderer::froxelDensityRejectReasonLabel(reason), "empty_storage") == 0,
               "tryValidate rejects undersized storage");
    expectTrue(reason == fuse::renderer::FroxelDensityRejectReason::DescMismatch,
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(oversized, desc, reason),
               "tryValidate rejects oversized storage");
    expectTrue(std::strcmp(fuse::renderer::froxelDensityRejectReasonLabel(reason), "desc_mismatch") == 0,

// --- deepen additive from deepen-b511-froxel-density-guards-e271 ---
void testFroxelTryClampAndDensityAccessGuards() {
               "trySample at origin succeeds");
    expectNear(sampledDensity, 1.25f, 1e-5f, "trySample at origin returns stored density");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 999u, clampedDensity),
               "trySample at OOB index succeeds");
    expectNear(clampedDensity, 2.75f, 1e-5f, "trySample at clamped last index returns stored density");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordDensity),
               "trySample at tile coords succeeds");
    expectNear(coordDensity, 1.25f, 1e-5f, "trySample at tile coords returns stored density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedDensity),
    expectNear(rejectedDensity, 0.f, 1e-6f, "trySample zeroes output on guard failure");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, mismatched, 0u, 0u, 0u, coordDensity),
               "trySample at coord rejects desc mismatch");
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(unclampedCoords, desc),
               "tryClamp sample coords clamps tile/slice corners");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(emptyCoords, zeroDesc),
    expectTrue(emptyCoords.tileX0 == 99u, "tryClamp leaves coords untouched on empty grid");

// --- deepen additive from deepen-b511-froxel-guards-8f53 ---
void testFroxelSampleCoordRejectAndEmptyGridGuards() {
    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, inBounds, coordReason),
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "none") == 0,
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, outOfRange, coordReason),
               "tryCanSampleAtCoords rejects OOB coords");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRange,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "out_of_range") == 0,
               "tryMapScreenDepthToSampleCoords succeeds in frustum");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthOutOfRange,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "depth_out_of_range") == 0,
               "tryMapScreenDepthToSampleCoords rejects empty froxel desc");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "empty_grid") == 0,
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(clamped, desc, coordReason),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, bilinearWithReason, coordReason),
               "trySampleDensityBilinear with reason rejects empty storage");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, indexSample, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectNear(indexSample, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns stored density");
               "trySampleDensityAtIndex with reason rejects empty storage");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DensityCountMismatch,
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "density_count_mismatch") == 0,

// --- deepen additive from deepen-b511-froxel-density-guards-ca9c ---
void testFroxelSampleCoordRejectAndPopulateGuards() {
               "tryMap sample coords succeeds on valid request");
               "tryMap sample coords rejects empty grid");
               "tryMap sample coords rejects invalid depth");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::InvalidDepth,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "invalid_depth") == 0,
               "tryMap froxel index succeeds on valid request");
    expectTrue(froxelIndex < desc.froxelCount(), "tryMap froxel index in bounds");
    expectTrue(fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(inBounds, desc, coordReason),
               "tryAreSampleCoordsInBounds accepts in-bounds coords");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(outOfBounds, desc, coordReason),
               "tryAreSampleCoordsInBounds rejects OOB tile coord");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "out_of_bounds") == 0,
    fuse::renderer::DensityLookupRejectReason coordLookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, desc, 1u, 1u, 2u, coordLookupReason),
               "tryCanLookupAtCoord accepts accessible grid at coord");
    expectTrue(coordLookupReason == fuse::renderer::DensityLookupRejectReason::None,
void testFroxelDensityLookupRejectAndValidationGuards() {
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled, lookupReason),
               "trySample at index with reason succeeds");
    expectNear(sampled, 1.f, 1e-5f, "trySample at index with reason returns density");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, coords, bilinear, lookupReason),
               "trySample bilinear with reason succeeds");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, coords, trilinear, lookupReason),
               "trySample trilinear with reason succeeds");
               "trySample at screen with reason succeeds");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, sampled, lookupReason),
               "trySample at index with reason rejects empty storage");
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "tryValidateGridDensityForDesc accepts accessible grid");
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, mismatched, densityReason),
               "tryValidateGridDensityForDesc rejects desc mismatch");
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, desc, camera, params),
               "tryPopulate succeeds on non-empty desc");
    expectTrue(populated.matchesDesc(desc), "tryPopulate allocates matching grid");
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, zeroDesc, camera, params),
               "tryPopulate rejects empty froxel desc");

// --- deepen additive from deepen-froxel-volumetrics-a5a0 ---
void testFroxelSampleCoordNormalizeAndBuildGuards() {
void testFroxelDensityLookupBoundsAndPreflightGuards() {
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndexBounds(grid, desc, 5u, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndexBounds(grid, desc, 999u, lookupReason),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndexBounds(grid, desc, 5u, boundedSample, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndexBounds(grid, desc, 999u, boundedSample, lookupReason),
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtSampleCoords(grid, desc, inBounds, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtSampleCoords(grid, desc, outOfRange, lookupReason),
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::SampleCoordsOutOfRange,
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinearAtCoords(
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinearAtCoords(
    const fuse::renderer::FroxelGridPreflight preflight =
        fuse::renderer::froxel_util::preflightFroxelDensityGrid(grid, desc);
    const fuse::renderer::FroxelGridPreflight emptyPreflight =
        fuse::renderer::froxel_util::preflightFroxelDensityGrid(grid, zeroDesc);
    expectTrue(!emptyPreflight.desc_non_empty, "preflight marks empty desc");
    expectTrue(!emptyPreflight.can_lookup(), "empty desc fails lookup preflight");
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityStrict(grid, zeroDesc, densityReason),
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::EmptyDesc,
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "empty_desc") == 0,
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensity(grid, zeroDesc, densityReason),
    const fuse::renderer::FroxelPopulatePreflight populatePreflight =
        fuse::renderer::froxel_util::preflightPopulateFroxelGrid(desc, params);
    expectTrue(populatePreflight.desc_non_empty, "populate preflight sees non-empty desc");
    expectTrue(populatePreflight.params_enabled, "populate preflight sees enabled params");
    expectTrue(populatePreflight.can_populate(), "populate preflight allows analytic populate");
    const fuse::renderer::FroxelPopulatePreflight disabledPreflight =
        fuse::renderer::froxel_util::preflightPopulateFroxelGrid(desc, disabled);
    expectTrue(!disabledPreflight.params_enabled, "populate preflight rejects zero density");
    expectTrue(!disabledPreflight.can_populate(), "populate preflight blocks disabled params");
    testFroxelDensityLookupBoundsAndPreflightGuards();

// --- deepen additive from deepen-b511-froxel-guards-0e8b ---
void testFroxelDensityValidationAndSampleGuardDeepen() {
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc vacuously succeeds on empty desc");
    expectTrue(fuse::renderer::froxel_util::tryValidateSampleCoords(inBounds, desc, coordReason),
               "tryValidateSampleCoords accepts in-bounds coords");
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(outOfRange, desc, coordReason),
               "tryValidateSampleCoords rejects OOB interpolation weight");
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(inBounds, zeroDesc, coordReason),
               "tryValidateSampleCoords rejects empty froxel desc");
    expectTrue(fuse::renderer::froxel_util::tryValidateSampleCoords(normalized, desc, coordReason),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, reasonSample, lookupReason),
               "trySample at index with reason succeeds on accessible grid");
    expectNear(reasonSample, 1.f, 1e-5f, "trySample with reason returns written density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedReasonSample,
    expectNear(rejectedReasonSample, 0.f, 1e-6f, "trySample with reason zeroes output on failure");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordReasonSample,
               "trySample at coord with reason succeeds on accessible grid");
    expectNear(coordReasonSample, 1.f, 1e-5f, "trySample at coord with reason returns origin density");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, bilinearReasonSample,
               "trySampleDensityAtScreen with reason matches unguarded screen sample");

// --- deepen additive from froxel-volumetric-guards-000f ---
void testFroxelDeepenGuardPreflights() {
    fuse::renderer::FroxelCameraRejectReason cameraReason = fuse::renderer::FroxelCameraRejectReason::None;
    expectTrue(fuse::renderer::FroxelSliceLayout::tryValidateCamera(camera, cameraReason),
               "tryValidateCamera accepts valid camera");
    expectTrue(cameraReason == fuse::renderer::FroxelCameraRejectReason::None, "valid camera reports no reject reason");
    expectTrue(!fuse::renderer::FroxelSliceLayout::tryValidateCamera(invalidNear, cameraReason),
               "tryValidateCamera rejects zero near plane");
    expectTrue(cameraReason == fuse::renderer::FroxelCameraRejectReason::InvalidNearPlane,
    expectTrue(std::strcmp(fuse::renderer::froxelCameraRejectReasonLabel(cameraReason), "invalid_near_plane") == 0,
    expectTrue(!fuse::renderer::FroxelSliceLayout::tryValidateCamera(invertedRange, cameraReason),
               "tryValidateCamera rejects inverted depth range");
    expectTrue(cameraReason == fuse::renderer::FroxelCameraRejectReason::InvertedDepthRange,
    fuse::renderer::FroxelGridRejectReason gridReason = fuse::renderer::FroxelGridRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateFroxelGridDesc(desc, gridReason),
    expectTrue(gridReason == fuse::renderer::FroxelGridRejectReason::None, "valid desc reports no grid reject reason");
    expectTrue(!fuse::renderer::froxel_util::tryValidateFroxelGridDesc(zeroY, gridReason),
    expectTrue(gridReason == fuse::renderer::FroxelGridRejectReason::EmptyTilesY,
    expectTrue(std::strcmp(fuse::renderer::froxelGridRejectReasonLabel(gridReason), "empty_tiles_y") == 0,
    fuse::renderer::FroxelScreenMappingRejectReason mappingReason =
        fuse::renderer::FroxelScreenMappingRejectReason::None;
               "tryMapScreenDepthToSampleCoords succeeds on valid inputs");
    expectTrue(mappingReason == fuse::renderer::FroxelScreenMappingRejectReason::None,
               "tryMap rejects depth below near plane");
    expectTrue(mappingReason == fuse::renderer::FroxelScreenMappingRejectReason::DepthBelowNear,
    expectTrue(std::strcmp(fuse::renderer::froxelScreenMappingRejectReasonLabel(mappingReason), "depth_below_near") ==
               "tryMap rejects invalid camera");
    expectTrue(mappingReason == fuse::renderer::FroxelScreenMappingRejectReason::InvalidCamera,
               "tryMapScreenDepthToFroxelIndex succeeds on valid inputs");
    expectTrue(fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(inBounds, desc, coordReason),
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(badTile, desc, coordReason),
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::TileOutOfRange,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "tile_out_of_range") == 0,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(badWeight, desc, coordReason),
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::WeightOutOfRange,
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndexInRange(grid, desc, 0u, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndexInRange(grid, desc, 999u, lookupReason),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinearInBounds(
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinearInBounds(
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(grid, mismatched, densityReason),
    fuse::renderer::FroxelGridRejectReason populateGridReason = fuse::renderer::FroxelGridRejectReason::None;
    fuse::renderer::FroxelCameraRejectReason populateCameraReason = fuse::renderer::FroxelCameraRejectReason::None;
               "tryPopulate succeeds on valid inputs");
    expectTrue(populateGrid.density.size() == desc.froxelCount(), "tryPopulate fills froxel grid");
               "tryPopulate rejects empty grid desc without modifying grid");
    expectTrue(populateGridReason == fuse::renderer::FroxelGridRejectReason::EmptyTilesY,
    testFroxelDeepenGuardPreflights();

// --- deepen additive from deepen-b511-froxel-guards-98ed ---
void testFroxelSampleCoordNormalizeAndPreflightGuards() {
    expectTrue(fuse::renderer::FroxelGridLayout::tryCanSampleAtCoords(valid, desc, coordReason),
               "tryCanSampleAtCoords succeeds on valid coords");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryCanSampleAtCoords(inverted, desc, coordReason),
               "tryCanSampleAtCoords rejects inverted corners");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::InvertedCorners,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "inverted_corners") == 0,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryCanSampleAtCoords(outOfRange, desc, coordReason),
               "tryCanSampleAtCoords rejects OOB tile coord");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryCanSampleAtCoords(valid, zeroDesc, coordReason),
               "tryCanSampleAtCoords rejects empty froxel desc");
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, valid, lookupReason, coordReason),
               "tryCanSampleAtCoords succeeds when grid and coords are valid");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(emptyGrid, desc, valid, lookupReason, coordReason),
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, outOfRange, lookupReason, coordReason),
               "tryCanSampleAtCoords rejects OOB sample coords");
                   fuse::renderer::GridDensityRejectReason::EmptyDesc),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, indexedSample, lookupReason),
    expectNear(indexedSample, 1.25f, 1e-5f, "trySampleDensityAtIndex with reason returns stored density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedIndexed, lookupReason),
    expectNear(rejectedIndexed, 0.f, 1e-6f, "trySampleDensityAtIndex with reason zeroes output on rejection");
    testFroxelSampleCoordNormalizeAndPreflightGuards();

// --- deepen additive from deepen-b511-froxel-guards-7caf ---
void testFroxelSampleCoordAndDensityPreflightGuards() {
               "tryMapScreenDepthToSampleCoords rejects depth below near");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthBelowNear,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "depth_below_near") == 0,
               "tryMapScreenDepthToSampleCoords rejects depth above far");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthAboveFar,
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, inBounds, lookupReason, coordReason),
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, oobCoords, lookupReason, coordReason),
    expectNear(sampled, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns stored density");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 2.5f, lookupReason),
    expectNear(coordSample, 1.f, 1e-5f, "trySampleDensityAtCoord with reason returns stored density");
               "trySampleDensityBilinear with reason succeeds on valid coords");
               "trySampleDensityTrilinear with reason succeeds on valid coords");
               "trySampleDensityAtScreen with reason rejects below-near depth");
               "tryValidateGridDensityForDesc succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(undersized, desc, densityReason),
               "tryValidateGridDensityForDesc rejects undersized storage");
    testFroxelSampleCoordAndDensityPreflightGuards();

// --- deepen additive from deepen-froxel-volumetric-guards-1251 ---
void testFroxelSampleCoordPreflightAndEmptyGridValidation() {
    expectTrue(!fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(badTile, desc, coordReason),
               "tryAreSampleCoordsInBounds rejects OOB tile");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRangeTile,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "out_of_range_tile") == 0,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(badWeight, desc, coordReason),
               "tryAreSampleCoordsInBounds rejects OOB interpolation weight");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRangeWeight,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(inBounds, zeroDesc, coordReason),
               "tryAreSampleCoordsInBounds rejects empty froxel grid");
               "tryMapScreenDepthToSampleCoords succeeds on valid input");
               "tryMapScreenDepthToFroxelIndex succeeds on valid input");
    expectTrue(froxelIndex < desc.froxelCount(), "tryMapScreenDepthToFroxelIndex yields in-bounds index");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
               "tryMapScreenDepthToFroxelIndex rejects invalid camera");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::InvalidCamera,
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGridForValidation, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc vacuously succeeds when both grid and desc are empty");
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc rejects non-empty storage on empty desc");
    expectNear(sampled, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, oobCoords, rejectedBilinear,
               "trySampleDensityBilinear with reason rejects OOB sample coords");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::SampleCoordRejected,
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "sample_coord_rejected") == 0,
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::ScreenMappingFailed,
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "screen_mapping_failed") == 0,
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(emptyGrid, desc, inBounds, rejectedBilinear,
               "trySampleDensityTrilinear with reason rejects empty storage");
    testFroxelSampleCoordPreflightAndEmptyGridValidation();

// --- deepen additive from deepen-b511-froxel-guards-1cb0 ---
void testFroxelSampleCoordMappingPreflightGuards() {
    fuse::renderer::SampleCoordRejectReason reason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::None,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "none") == 0,
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::DepthBelowNear,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "depth_below_near") == 0,
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::DepthAboveFar,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "depth_above_far") == 0,
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "empty_grid") == 0,
               "tryMapScreenDepthToFroxelIndex rejects below-near depth");
    expectTrue(rejectedIndex == 0u, "tryMapScreenDepthToFroxelIndex zeroes output on rejection");
void testFroxelSampleCoordBoundsPreflightGuards() {
    fuse::renderer::SampleCoordBoundsRejectReason boundsReason =
        fuse::renderer::SampleCoordBoundsRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateSampleCoords(inBounds, desc, boundsReason),
    expectTrue(boundsReason == fuse::renderer::SampleCoordBoundsRejectReason::None,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordBoundsRejectReasonLabel(boundsReason), "none") == 0,
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(oobTile, desc, boundsReason),
               "tryValidateSampleCoords rejects OOB tile coord");
    expectTrue(boundsReason == fuse::renderer::SampleCoordBoundsRejectReason::OutOfBoundsTile,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordBoundsRejectReasonLabel(boundsReason), "out_of_bounds_tile") ==
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(oobWeight, desc, boundsReason),
    expectTrue(boundsReason == fuse::renderer::SampleCoordBoundsRejectReason::OutOfBoundsWeight,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordBoundsRejectReasonLabel(boundsReason),
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(inBounds, zeroDesc, boundsReason),
               "tryValidateSampleCoords rejects empty grid desc");
    expectTrue(boundsReason == fuse::renderer::SampleCoordBoundsRejectReason::EmptyGrid,
void testFroxelDensityLookupAndValidationPreflightGuards() {
    expectNear(sampled, 1.25f, 1e-5f, "trySampleDensityAtIndex with reason returns written density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedSample, lookupReason),
    expectNear(rejectedSample, 0.f, 1e-6f, "trySampleDensityAtIndex with reason zeroes output on failure");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(emptyGrid, desc, 0u, 9.f, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, mismatched, 0u, 9.f, lookupReason),
               "tryWriteDensityAtIndex with reason rejects desc mismatch");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, coords, bilinearSample, lookupReason),
               "trySampleDensityAtScreen with reasons succeeds on accessible grid");
               "trySampleDensityAtScreen with reasons rejects below-near depth");
    expectNear(rejectedScreen, 0.f, 1e-6f, "trySampleDensityAtScreen zeroes output on mapping failure");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::DepthBelowNear,
    testFroxelSampleCoordMappingPreflightGuards();
    testFroxelSampleCoordBoundsPreflightGuards();
    testFroxelDensityLookupAndValidationPreflightGuards();

// --- deepen additive from deepen-froxel-volumetric-guards-8201 ---
void testFroxelSampleCoordPreflightAndNormalizeGuards() {
    expectTrue(fuse::renderer::tryMapScreenDepthToSampleCoords(0.25f, 0.5f, 10.f, desc, camera, mapped, coordReason),
               "tryMap succeeds on valid screen depth");
    expectTrue(!fuse::renderer::tryMapScreenDepthToSampleCoords(0.5f, 0.5f, 10.f, zeroDesc, camera, rejectedCoords,
               "tryMap rejects empty froxel grid");
    expectTrue(!fuse::renderer::tryMapScreenDepthToSampleCoords(0.5f, 0.5f, 0.01f, desc, camera, rejectedCoords,
    expectTrue(!fuse::renderer::tryMapScreenDepthToSampleCoords(0.5f, 0.5f, 200.f, desc, camera, rejectedCoords,
               "tryMap rejects depth above far plane");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, lookupSample, lookupReason),
    expectNear(lookupSample, 1.f, 1e-5f, "trySample at index with reason returns density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, zeroDesc, 0u, rejectedLookup, lookupReason),
               "trySample at index with reason rejects empty froxel desc");
               "trySample trilinear with reason succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(emptyGrid, desc, inBounds, rejectedTrilinear,
               "trySample trilinear with reason rejects empty storage");
    fuse::renderer::SampleCoordRejectReason screenCoordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f,
               "trySample at screen with reasons succeeds on accessible grid");
    expectTrue(screenCoordReason == fuse::renderer::SampleCoordRejectReason::None,
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(emptyGrid, desc, camera, 0.5f, 0.5f, 10.f,
               "trySample at screen with reasons rejects empty storage");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(grid, desc, camera, 0.5f, 0.5f, 0.01f,
               "trySample at screen with reasons rejects out-of-range depth");
    expectTrue(screenCoordReason == fuse::renderer::SampleCoordRejectReason::DepthOutOfRange,
void testFroxelGridDensityValidationForDescGuards() {
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, zeroDesc, densityReason),
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "undersized_storage") == 0,
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, desc, densityReason),
    testFroxelSampleCoordPreflightAndNormalizeGuards();

// --- deepen additive from deepen-b511-froxel-preflight-guards-73d5 ---
void testFroxelPreflightGuardsDeepen() {
    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelSample(grid, desc, inBounds),
               "wouldSkipFroxelSample does not skip accessible in-bounds sample");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelSample(grid, desc, invalidWeights),
               "wouldSkipFroxelSample still proceeds when weights will be clamped");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleReject(grid, desc, invalidWeights) ==
               "classifyFroxelSampleReject reports invalid_weights for OOB t");
    fuse::renderer::SampleCoordRejectReason skipReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelSample(emptyGrid, desc, inBounds, &skipReason),
               "wouldSkipFroxelSample skips empty storage");
    expectTrue(skipReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "empty_storage") == 0,
    expectNear(coordSample, 1.f, 1e-5f, "trySampleDensityAtCoord with reason returns density");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 3.25f, lookupReason),
               "trySampleDensityAtScreen with reason succeeds in range");
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, params, populateReason),
               "tryCanPopulateFromAnalyticFog accepts enabled params");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, zeroDensity, populateReason),
               "tryCanPopulateFromAnalyticFog rejects zero density");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, zeroMarch, populateReason),
               "tryCanPopulateFromAnalyticFog rejects zero march steps");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(zeroDesc, params, populateReason),
               "tryCanPopulateFromAnalyticFog rejects empty froxel desc");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::EmptyGrid,
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "empty_grid") == 0,
    testFroxelPreflightGuardsDeepen();

// --- deepen additive from deepen-froxel-volumetric-guards-d18a ---
void testFroxelPreflightValidateAndLookupReasonGuards() {
    expectTrue(fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(inBounds, desc, sampleReason),
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(reversed, desc, sampleReason),
               "tryValidateSampleCoords rejects unordered corners");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::UnorderedCorners,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(sampleReason), "unordered_corners") == 0,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(anyCoords, zeroDesc, sampleReason),
               "tryValidateSampleCoords rejects empty grid");
    fuse::renderer::GridDensityRejectReason gridReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightNonEmptyGrid(desc, gridReason),
               "tryPreflightNonEmptyGrid succeeds on non-empty grid");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::None,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightNonEmptyGrid(zeroDesc, gridReason),
               "tryPreflightNonEmptyGrid rejects empty grid");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::EmptyDesc,
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(gridReason), "empty_desc") == 0,
    expectTrue(fuse::renderer::froxel_util::tryPreflightDensityGridAccess(grid, desc, gridReason),
               "tryPreflightDensityGridAccess succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::tryPreflightDensityGridAccess(emptyGrid, desc, gridReason),
               "tryPreflightDensityGridAccess rejects empty storage");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
    expectTrue(!fuse::renderer::froxel_util::tryPreflightDensityGridAccess(grid, zeroDesc, gridReason),
               "tryPreflightDensityGridAccess rejects empty desc");
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, gridReason),
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, mismatched, gridReason),
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, zeroDesc, gridReason),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 3u, sampled, lookupReason),
    expectNear(sampled, 4.25f, 1e-5f, "trySampleDensityAtIndex with reason returns written density");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 5.5f, lookupReason),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 1u, 1u, 2u, coordSample, lookupReason),
    expectNear(coordSample, 5.5f, 1e-5f, "trySampleDensityAtCoord with reason returns written density");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, mismatched, 0u, 0u, 0u, 9.f, lookupReason),
               "tryWriteDensityAtCoord with reason rejects desc mismatch");
    testFroxelPreflightValidateAndLookupReasonGuards();

// --- deepen additive from deepen-b511-froxel-guards-2580 ---
void testFroxelPopulatePreflightAndIndexValidationGuards() {
               "tryCanPopulate succeeds on valid inputs");
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(grid, desc, camera, params, populateReason),
    expectTrue(grid.matchesDesc(desc), "tryPopulate allocates matching density storage");
               "tryPopulate fills all froxels with non-zero density");
    fuse::renderer::DensityLookupRejectReason indexReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateFroxelIndex(5u, desc, indexReason),
    expectTrue(indexReason == fuse::renderer::DensityLookupRejectReason::None,
    expectTrue(!fuse::renderer::froxel_util::tryValidateFroxelIndex(99u, desc, indexReason),
    expectTrue(indexReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(indexReason), "index_out_of_range") == 0,
    expectTrue(!fuse::renderer::froxel_util::tryValidateFroxelIndex(0u, zeroDesc, indexReason),
    expectTrue(indexReason == fuse::renderer::DensityLookupRejectReason::EmptyGrid,
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, indexedSample, indexReason),
    expectTrue(fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(validCoords, desc, sampleReason),
               "valid sample coords pass tryValidateSampleCoords");
               "reversed sample corners fail tryValidateSampleCoords");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(oobIndices, desc, sampleReason),
               "OOB tile indices fail tryValidateSampleCoords");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(oobWeights, desc, sampleReason),
               "OOB interpolation weights fail tryValidateSampleCoords");
    fuse::renderer::ScreenMappingRejectReason screenReason = fuse::renderer::ScreenMappingRejectReason::None;
    expectTrue(screenReason == fuse::renderer::ScreenMappingRejectReason::None,
    expectTrue(screenReason == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
    testFroxelPopulatePreflightAndIndexValidationGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-ecd6 ---
    expectTrue(fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(desc, camera, params, populateReason),
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(rejectedGrid, zeroDesc, camera, params),
               "tryPopulate rejects empty froxel desc without mutating storage");
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroDensity, populateReason),
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroMarch, populateReason),
    expectTrue(!fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(desc, badCamera, params, populateReason),
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(guardedGrid, desc, badCamera, params),
               "tryPopulate rejects invalid camera without filling density");

// --- deepen additive from deepen-froxel-volumetric-guards-a3b2 ---
void testFroxelPopulateLookupAndStrictSampleGuards() {
    fuse::renderer::PopulateRejectReason populateReason = fuse::renderer::PopulateRejectReason::None;
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::None,
    expectTrue(std::strcmp(fuse::renderer::populateRejectReasonLabel(populateReason), "none") == 0,
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::EmptyGrid,
    expectTrue(std::strcmp(fuse::renderer::populateRejectReasonLabel(populateReason), "empty_grid") == 0,
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::InvalidCamera,
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::ZeroDensity,
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::ZeroMarchSteps,
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(skipped, desc, camera, zeroDensity),
               "tryPopulate reports false when density fill is skipped");
    expectTrue(skipped.density.size() == desc.froxelCount(), "tryPopulate still allocates on skip path");
    expectNear(indexSample, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedIndexSample, lookupReason),
               "trySampleDensityAtScreen with reasons matches unguarded sample");
               "trySampleDensityAtScreen with reasons rejects empty storage");
               "trySampleDensityAtScreen with reasons rejects depth below near plane");
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoordsStrict(grid, desc, inBounds, sampleReason),
               "tryCanSampleAtCoordsStrict succeeds on valid coords");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoordsStrict(grid, desc, warnCoords, sampleReason),
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoordsStrict(grid, desc, oobCoords, sampleReason),

// --- deepen additive from deepen-froxel-b511-guards-9658 ---
void testFroxelCoordLookupPopulateAndCornerPreflightGuards() {
               "tryCanLookupAtCoord succeeds for in-range coords");
               "tryCanLookupAtCoord warns but succeeds for OOB coords");
               "trySampleDensityAtIndex with reason succeeds for origin");
    expectNear(indexSample, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns origin density");
               "trySampleDensityAtCoord with reason succeeds for origin");
    expectTrue(screenReason == fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "trySampleDensityAtScreen with reason rejects invalid camera");
    expectTrue(screenReason == fuse::renderer::ScreenMappingRejectReason::InvalidCamera,
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, desc, populateCamera, params,
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(skipped, desc, populateCamera, zeroDensity,
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(reversed, desc, sampleReason),
               "tryPreflightSampleCoords warns but succeeds for reversed corners");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::InvalidCorners,
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(sampleReason), "invalid_corners") == 0,
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(extremeWeights, desc, sampleReason),
               "tryPreflightSampleCoords warns but succeeds for OOB weights");
    testFroxelCoordLookupPopulateAndCornerPreflightGuards();

// --- deepen additive from deepen-b511-froxel-guards-62b9 ---
void testFroxelCoordLookupAndScreenSampleGuards() {
               "tryCanLookupAtCoord accepts accessible grid");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 3u, 1u, 2u, coordSample, lookupReason),
    expectNear(coordSample, 2.f, 1e-5f, "trySampleDensityAtCoord with reason returns last-cell density");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 0u, 1u, 3.25f, lookupReason),
    expectNear(rejectedIndexSample, 0.f, 1e-6f, "trySampleDensityAtIndex with reason zeroes output on rejection");
               "trySampleDensityAtScreen with reasons matches unguarded screen sample");
    expectNear(rejectedScreen, 0.f, 1e-6f, "trySampleDensityAtScreen with reasons zeroes output on rejection");
    expectTrue(std::strcmp(fuse::renderer::screenMappingRejectReasonLabel(screenReason), "depth_out_of_range") == 0,
               "tryPopulateFromAnalyticFog with reason succeeds for valid inputs");
               "tryPopulateFromAnalyticFog with reason fills all froxels on success");
               "tryPopulateFromAnalyticFog with reason returns false when preflight rejects fill");
    expectTrue(skipped.matchesDesc(desc), "tryPopulateFromAnalyticFog with reason still allocates on rejected fill");

// --- deepen additive from deepen-b511-froxel-guards-c397 ---
void testFroxelCoordLookupAndReasonOverloadGuards() {
               "tryCanLookupAtCoord warns but succeeds for clampable coords");
    expectNear(indexSample, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns written density");
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populateGrid, desc, badCamera, params,
               "tryPopulateFromAnalyticFog with reason rejects invalid camera");
    expectTrue(populateGrid.matchesDesc(desc), "tryPopulateFromAnalyticFog with reason still allocates on rejection");

// --- deepen additive from deepen-froxel-volumetrics-b511-4bac ---
void testFroxelDeepenedLookupPopulateAndScreenGuards() {
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, desc, 0u, 0u, 0u, lookupReason),
               "trySampleDensityAtCoord with reason succeeds for clampable coords");
    expectNear(oobCoordSample, 2.f, 1e-5f, "trySampleDensityAtCoord with reason clamps OOB coords");
               "trySampleDensityAtIndex with reason succeeds for clampable index");
    expectNear(indexSample, 2.f, 1e-5f, "trySampleDensityAtIndex with reason clamps OOB index");
               "trySampleDensityAtScreen with map reason rejects empty storage");
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populateGrid, desc, camera, params,
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(rejectedPopulate, desc, badCamera, zeroDensity,

// --- deepen additive from deepen-froxel-volumetrics-b511-037c ---
void testFroxelDeepenDensitySamplePopulateGuards() {
               "tryCanLookupAtCoord accepts in-bounds tile coords");
    fuse::renderer::SampleCoordRejectReason tileReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightTileCoords(1u, 1u, 2u, desc, tileReason),
               "tryPreflightTileCoords succeeds for in-bounds coords");
    expectTrue(tileReason == fuse::renderer::SampleCoordRejectReason::None,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightTileCoords(99u, 0u, 0u, desc, tileReason),
               "tryPreflightTileCoords rejects hard OOB tile coord");
    expectTrue(tileReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
    expectTrue(fuse::renderer::FroxelGridLayout::canPreflightTileCoords(0u, 0u, 0u, desc),
               "canPreflightTileCoords succeeds for origin");
    expectNear(rejectedIndexSample, 0.f, 1e-6f, "trySampleDensityAtIndex zeroes output on rejection");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 3.25f, lookupReason),
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 4.75f, lookupReason),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 1u, 1u, 2u, writtenCoord, lookupReason),
               "trySampleDensityAtCoord reads back written coord density");
    expectNear(writtenCoord, 4.75f, 1e-5f, "tryWriteDensityAtCoord with reason persists density");
               "trySampleDensityAtScreen with reason succeeds for in-bounds depth");
    expectTrue(populated.matchesDesc(desc), "tryPopulateFromAnalyticFog with reason allocates matching grid");
               "tryPopulateFromAnalyticFog with reason rejects zero density");
    expectTrue(skippedPopulate.matchesDesc(desc), "tryPopulateFromAnalyticFog still allocates on rejected fill");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(tileReason), "out_of_bounds") == 0,
                   fuse::renderer::DensityLookupRejectReason::SampleCoordRejected),

// --- deepen additive from deepen-froxel-b511-guards-e86c ---
void testFroxelCoordLookupAndGuardDiagnostics() {
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, sampled, lookupReason),
    expectNear(sampled, 1.f, 1e-5f, "trySampleDensityAtCoord with reason returns origin density");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 99u, 99u, 99u, oobSample, lookupReason),
    expectNear(oobSample, 2.f, 1e-5f, "trySampleDensityAtCoord with reason clamps OOB coords to last cell");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 1u, 1u, 2u, written, lookupReason),
    expectNear(written, 3.75f, 1e-5f, "tryWriteDensityAtCoord with reason persists density");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtCoord(emptyGrid, desc, 0u, 0u, 0u, 1.f, lookupReason),
               "tryWriteDensityAtCoord with reason rejects empty storage");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 4.5f, lookupReason),
               "trySampleDensityAtScreen with reason rejects empty froxel desc");

// --- deepen additive from deepen-froxel-volumetrics-b511-425b ---
void testFroxelCoordLookupScreenSampleAndPopulateReasonGuards() {
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::CoordOutOfRange,
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "coord_out_of_range") == 0,
               "trySampleDensityAtIndex with reason succeeds at origin");
               "trySampleDensityAtIndex with reason succeeds for clampable OOB index");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 5u, writtenIndex),
               "written index readable after tryWrite with reason");
    expectNear(writtenIndex, 3.25f, 1e-5f, "tryWriteDensityAtIndex with reason persists density");
               "trySampleDensityAtCoord with reason succeeds at origin");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 99u, 99u, 99u, coordSample, lookupReason),
               "trySampleDensityAtCoord with reason succeeds for clampable OOB coords");
    expectNear(coordSample, 2.f, 1e-5f, "trySampleDensityAtCoord with reason clamps OOB coords");
    expectTrue(populated.matchesDesc(desc), "tryPopulate with reason allocates matching grid");

// --- deepen additive from deepen-b511-froxel-guards-b0a8 ---
void testFroxelCoordLookupScreenSampleAndDescGuards() {
    expectTrue(std::strcmp(fuse::renderer::screenMappingRejectReasonLabel(mapReason), "depth_out_of_range") == 0,
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelSample(grid, desc, hardOob),
               "wouldSkipFroxelSample true for hard OOB coords");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelSample(grid, desc, hardOob, &sampleReason),
               "wouldSkipFroxelSample reports reason for hard OOB coords");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelSample(grid, desc, warnWeights),
               "wouldSkipFroxelSample false when weights will be clamped");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleReject(grid, desc, warnWeights) ==
               "classifyFroxelSampleReject reports invalid_weights for clampable weights");

// --- deepen additive from deepen-b511-froxel-guards-c46a ---
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, desc.maxFroxelIndex(), 4.25f,
               "trySampleDensityAtScreen rejects depth below near plane");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(emptyGrid, zeroDesc, camera, 0.5f, 0.5f, 10.f,
               "trySampleDensityAtScreen rejects empty grid desc");

// --- deepen additive from deepen-froxel-b511-guards-79a2 ---
void testFroxelCoordLookupAndRejectReasonGuards() {
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 999u, oobSample, lookupReason),
               "trySampleDensityAtIndex with reason succeeds when index will clamp");
    expectNear(oobSample, 2.f, 1e-5f, "trySampleDensityAtIndex with reason clamps OOB index");
    expectNear(rejectedSample, 0.f, 1e-6f, "trySampleDensityAtIndex with reason zeroes output on rejection");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 2.75f, lookupReason),
               "trySampleDensityAtCoord with reason succeeds when coords will clamp");
    testFroxelCoordLookupAndRejectReasonGuards();

// --- deepen additive from deepen-b511-froxel-guards-ea5a ---
void testFroxelCoordLookupPopulateAndScreenGuards() {
               "trySampleDensityAtCoord with reason succeeds in range");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 99u, 99u, 99u, clampedCoordSample,
    expectNear(clampedCoordSample, 2.f, 1e-5f, "trySampleDensityAtCoord clamps OOB coords to last cell");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 1u, 3.25f, lookupReason),
               "tryWriteDensityAtCoord with reason succeeds in range");

// --- deepen additive from deepen-froxel-preflight-guards-4be4 ---
void testFroxelCoordLookupDescPopulationAndDiagnosticGuards() {
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, desc, 99u, 0u, 0u, lookupReason),
               "tryCanLookupAtCoord warns but succeeds for clampable OOB tileX");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 0u, 0u, 2.5f, lookupReason),
               "tryPopulate with reason rejects invalid camera");
               "tryPopulate with reason reports invalid_camera populate reject reason");

// --- deepen additive from deepen-froxel-b511-guards-9ea0 ---
void testFroxelValidateCoordsDensitySizingAndTrilinearGuards() {
    fuse::renderer::FroxelSampleCoordsRejectReason validateReason =
        fuse::renderer::FroxelSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(valid, desc, validateReason),
    expectTrue(validateReason == fuse::renderer::FroxelSampleCoordsRejectReason::None,
    expectTrue(std::strcmp(fuse::renderer::froxelSampleCoordsRejectReasonLabel(validateReason), "none") == 0,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(reversed, desc, validateReason),
               "unordered corners fail tryValidateSampleCoords");
    expectTrue(validateReason == fuse::renderer::FroxelSampleCoordsRejectReason::UnorderedCorners,
    expectTrue(std::strcmp(fuse::renderer::froxelSampleCoordsRejectReasonLabel(validateReason), "unordered_corners") ==
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(oobIndices, desc, validateReason),
               "OOB indices fail tryValidateSampleCoords");
    expectTrue(validateReason == fuse::renderer::FroxelSampleCoordsRejectReason::OutOfRangeIndices,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(oobWeights, desc, validateReason),
               "OOB weights fail tryValidateSampleCoords");
    expectTrue(validateReason == fuse::renderer::FroxelSampleCoordsRejectReason::OutOfRangeWeights,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(valid, emptyDesc, validateReason),
               "empty desc fails tryValidateSampleCoords");
    expectTrue(validateReason == fuse::renderer::FroxelSampleCoordsRejectReason::EmptyGrid,
    expectTrue(fuse::renderer::froxel_util::tryValidateDensityLookupIndex(grid, desc, 5u, lookupReason),
               "in-range index passes tryValidateDensityLookupIndex");
    expectTrue(!fuse::renderer::froxel_util::tryValidateDensityLookupIndex(grid, desc, 999u, lookupReason),
               "OOB index fails tryValidateDensityLookupIndex");
    expectTrue(!fuse::renderer::froxel_util::tryValidateDensityLookupIndex(undersized, desc, 0u, lookupReason),
               "undersized storage fails tryValidateDensityLookupIndex");
    expectTrue(!fuse::renderer::froxel_util::tryValidateDensityLookupIndex(emptyGrid, desc, 0u, lookupReason),
               "empty storage fails tryValidateDensityLookupIndex");
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, valid, trilinearReason),
               "tryCanSampleAtCoords with trilinear reason succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(emptyGrid, desc, valid, trilinearReason),
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::EmptyStorage,
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(undersized, desc, valid, trilinearReason),
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::UndersizedStorage,
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(oversized, desc, valid, trilinearReason),
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::DescMismatch,
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, oobIndices, trilinearReason),
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, emptyDesc, valid, trilinearReason),
               "wouldSkipFroxelPopulate false for valid populate inputs");
               "wouldSkipFroxelPopulate true for zero density");
               "wouldSkipFroxelPopulate mirrors !canPopulateFromAnalyticFog");
               "wouldSkipFroxelPopulate true for invalid camera");

// --- deepen additive from deepen-froxel-volumetrics-b511-53be ---
void testFroxelCoordLookupScreenPopulateAndTrilinearGuards() {
               "tryCanLookupAtCoord succeeds for in-bounds coords");
               "trySampleDensityAtCoord with reason succeeds for in-bounds coords");
    expectNear(coordSample, 3.5f, 1e-5f, "trySampleDensityAtCoord with reason returns written density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtCoord(emptyGrid, desc, 0u, 0u, 0u, rejectedCoord, lookupReason),
               "trySampleDensityAtCoord with reason rejects empty storage");
    fuse::renderer::DensityLookupRejectReason screenLookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(screenLookupReason == fuse::renderer::DensityLookupRejectReason::None,
    expectTrue(screenLookupReason == fuse::renderer::DensityLookupRejectReason::ScreenMappingFailed,
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(screenLookupReason),
    expectTrue(fuse::renderer::froxel_util::tryCanSampleDensityTrilinear(grid, desc, inBounds, trilinearReason),
               "tryCanSampleDensityTrilinear succeeds for in-bounds coords");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleDensityTrilinear(grid, desc, hardOob, trilinearReason),
               "tryCanSampleDensityTrilinear rejects hard OOB coords");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleDensityTrilinear(emptyGrid, desc, inBounds, trilinearReason),
               "tryCanSampleDensityTrilinear rejects empty storage");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::LookupFailed,
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, hardOob, rejectedTrilinear, sampleReason),
               "trySampleDensityTrilinear rejects hard OOB coords");
    expectNear(rejectedTrilinear, 0.f, 1e-6f, "trySampleDensityTrilinear zeroes output on hard OOB rejection");

// --- deepen additive from deepen-b511-froxel-preflights-8549 ---
void testFroxelDeepenPreflightGuards() {
    expectTrue(fuse::renderer::froxel_util::tryPreflightDensityLookup(grid, desc, 0u, lookupReason),
               "tryPreflightDensityLookup succeeds on accessible grid");
               "preflightDensityLookup succeeds with optional reason");
    expectTrue(fuse::renderer::froxel_util::tryPreflightDensityLookup(grid, desc, 999u, lookupReason),
               "tryPreflightDensityLookup warns but succeeds for OOB index");
    expectTrue(!fuse::renderer::froxel_util::tryPreflightDensityLookup(emptyGrid, desc, 0u, lookupReason),
               "tryPreflightDensityLookup rejects empty storage");
    expectTrue(!fuse::renderer::FroxelGridLayout::shouldSkipSampleCoordPreflight(desc),
               "preflightSampleCoords warns but succeeds for clampable weights");
    expectTrue(fuse::renderer::FroxelGridLayout::shouldSkipSampleCoordPreflight(zeroDesc),
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightSampleCoords(inBounds, zeroDesc, &sampleReason),
               "preflightSampleCoords rejects empty desc");
    expectTrue(fuse::renderer::froxel_util::canPreflightTrilinearSample(grid, desc, inBounds),
               "canPreflightTrilinearSample succeeds for accessible grid");
    expectTrue(fuse::renderer::froxel_util::tryPreflightTrilinearSample(grid, desc, inBounds, trilinearReason),
               "tryPreflightTrilinearSample succeeds for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::tryPreflightTrilinearSample(grid, desc, warnWeights, trilinearReason),
               "tryPreflightTrilinearSample warns but succeeds for clampable weights");
    expectTrue(!fuse::renderer::froxel_util::tryPreflightTrilinearSample(grid, desc, hardOob, trilinearReason),
               "tryPreflightTrilinearSample rejects hard OOB coords");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::OutOfBoundsCoords,
    expectTrue(!fuse::renderer::froxel_util::tryPreflightTrilinearSample(emptyGrid, desc, inBounds, trilinearReason),
               "tryPreflightTrilinearSample rejects inaccessible grid");
    expectTrue(fuse::renderer::froxel_util::tryPreflightFroxelPopulate(desc, camera, params, populateReason),
               "tryPreflightFroxelPopulate succeeds for valid inputs");
               "preflightFroxelPopulate succeeds with optional reason");
    expectTrue(!fuse::renderer::froxel_util::tryPreflightFroxelPopulate(desc, camera, zeroDensity, populateReason),
               "tryPreflightFroxelPopulate rejects zero density");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(emptyDesc, camera, params, &populateReason),
               "preflightFroxelPopulate rejects empty desc");
    testFroxelDeepenPreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-c280 ---
void testFroxelTrilinearAndCoordLookupDeepGuards() {
    expectTrue(fuse::renderer::froxel_util::tryCanSampleTrilinear(grid, desc, inBounds, trilinearReason),
               "tryCanSampleTrilinear succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::tryCanSampleTrilinear(grid, desc, warnWeights, trilinearReason),
               "tryCanSampleTrilinear still succeeds when weights will be clamped");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinear(grid, desc, hardOob, trilinearReason),
               "tryCanSampleTrilinear rejects hard OOB tile coord");
               "trySampleDensityAtScreen with trilinear reason succeeds on accessible grid");
               "trySampleDensityAtScreen with trilinear reason matches unguarded screen sample");
               "trySampleDensityAtScreen with trilinear reason rejects empty storage");
               "trySampleDensityAtScreen with trilinear reason rejects depth below near plane");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::ScreenMappingFailed,
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinear(grid, zeroDesc, inBounds, trilinearReason),
               "tryCanSampleTrilinear rejects empty desc");
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(invalidCameraGrid, desc, badCamera, params,

// --- deepen additive from deepen-froxel-volumetrics-b511-527f ---
void testFroxelTrilinearAndLookupDeepenGuards() {
               "tryCanLookupAtCoord warns but succeeds for OOB tile/slice coords");
               "tryPreflightSampleCoords warns but succeeds for unordered corners");
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(clamped, desc, sampleReason),
               "tryClampSampleCoords with reason succeeds on non-empty grid");
               "tryClampSampleCoords produces valid sample coords");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(unchanged, zeroDesc, sampleReason),
               "tryClampSampleCoords with reason rejects empty grid");
               "tryCanSampleDensityTrilinear accepts accessible grid");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleDensityTrilinear(grid, zeroDesc, inBounds, trilinearReason),
               "tryCanSampleDensityTrilinear rejects empty desc");
               "trySampleDensityTrilinear with trilinear reason rejects empty storage");
               "trySampleDensityAtScreen with map reason succeeds in range");
               "trySampleDensityAtScreen with map reason matches unguarded sample");
               "trySampleDensityAtScreen with map reason rejects below-near depth");
               "trySampleDensityAtScreen with trilinear reason rejects below-near depth");

// --- deepen additive from deepen-froxel-volumetrics-b511-da24 ---
void testFroxelClassifyAndTrilinearPreflightGuards() {
    expectTrue(fuse::renderer::classifyFroxelDensityLookupReject(grid, desc, 0u) ==
    expectTrue(fuse::renderer::classifyFroxelDensityLookupReject(grid, desc, 999u) ==
    expectTrue(fuse::renderer::classifyFroxelDensityLookupReject(emptyGrid, desc, 0u) ==
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelDensityLookup(emptyGrid, desc, &lookupReason),
               "wouldSkip density lookup true for empty storage");
               "wouldSkip density lookup reports empty_storage reason");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelDensityLookup(grid, desc, &lookupReason),
               "wouldSkip density lookup false for accessible grid");
               "wouldSkip density lookup reports none on accessible grid");
    expectTrue(fuse::renderer::classifyFroxelSampleCoordReject(inBounds, desc) ==
    expectTrue(fuse::renderer::classifyFroxelSampleCoordReject(warnWeights, desc) ==
    expectTrue(fuse::renderer::classifyFroxelSampleCoordReject(reversed, desc) ==
                               fuse::renderer::SampleCoordRejectReason::UnorderedCorners),
    fuse::renderer::SampleCoordRejectReason clampReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(clamped, desc, clampReason),
    expectTrue(clampReason == fuse::renderer::SampleCoordRejectReason::None,
               "tryClampSampleCoords with reason reports none on success");
               "tryClampSampleCoords with reason produces valid coords");
    expectTrue(fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, inBounds, trilinearReason),
               "tryCanSampleTrilinearAtCoords succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, reversed, trilinearReason),
               "tryCanSampleTrilinearAtCoords rejects unordered corners");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(emptyGrid, desc, inBounds, trilinearReason),
               "tryCanSampleTrilinearAtCoords rejects empty storage");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::NotAccessible,
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(undersized, desc, inBounds, trilinearReason),
               "tryCanSampleTrilinearAtCoords rejects undersized storage");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, inBounds, strictTrilinear, trilinearReason),
               "trySampleDensityTrilinear with trilinear reason succeeds on valid coords");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, reversed, rejectedStrict, trilinearReason),
               "trySampleDensityTrilinear with trilinear reason rejects unordered corners");
    expectNear(rejectedStrict, 0.f, 1e-6f, "trySampleDensityTrilinear with trilinear reason zeroes output on rejection");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelTrilinearSample(grid, desc, reversed, &trilinearReason),
               "wouldSkip trilinear sample true for invalid coords");
               "wouldSkip trilinear sample reports invalid_sample_coords reason");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelTrilinearSample(grid, desc, inBounds, &trilinearReason),
               "wouldSkip trilinear sample false for valid coords");
    expectTrue(fuse::renderer::classifyFroxelTrilinearSampleReject(emptyGrid, zeroDesc, inBounds) ==
    expectTrue(fuse::renderer::classifyFroxelPopulateReject(desc, camera, params) ==
               "wouldSkip populate false for valid inputs");
    expectTrue(fuse::renderer::classifyFroxelPopulateReject(desc, camera, zeroDensity) ==
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroDensity, &populateReason),
               "wouldSkip populate true for zero density");
               "wouldSkip populate reports zero_density reason");
    expectTrue(fuse::renderer::classifyFroxelScreenMappingReject(0.5f, 0.5f, 10.f, desc, camera) ==
    expectTrue(fuse::renderer::classifyFroxelScreenMappingReject(0.5f, 0.5f, 0.01f, desc, camera) ==
    expectTrue(fuse::renderer::classifyFroxelScreenMappingReject(0.5f, 0.5f, 10.f, zeroDesc, camera) ==
    testFroxelClassifyAndTrilinearPreflightGuards();

// --- deepen additive from deepen-b511-froxel-guards-874b ---
void testFroxelTrilinearAndStrictLookupGuards() {
    expectTrue(fuse::renderer::FroxelGridLayout::tryNormalizeAndPreflightSampleCoords(normalized, desc, coordReason),
               "tryCanSampleTrilinear still succeeds when weights will clamp");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::ClampRequired,
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason), "clamp_required") ==
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinear(emptyGrid, desc, inBounds, trilinearReason),
               "tryCanSampleTrilinear rejects empty storage");
    expectTrue(fuse::renderer::froxel_util::tryPreflightStrictDensityLookupAtIndex(grid, desc, 0u, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::tryPreflightStrictDensityLookupAtIndex(grid, desc, 999u, lookupReason),
    expectTrue(fuse::renderer::froxel_util::wouldRejectDensityLookupAtIndex(grid, desc, 999u),
    expectTrue(!fuse::renderer::froxel_util::wouldRejectDensityLookupAtIndex(grid, desc, 0u),
    expectTrue(fuse::renderer::froxel_util::tryPreflightStrictDensityLookupAtCoord(grid, desc, 1u, 1u, 2u, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::tryPreflightStrictDensityLookupAtCoord(grid, desc, 99u, 99u, 99u,
    expectTrue(fuse::renderer::froxel_util::wouldRejectDensityLookupAtCoord(grid, desc, 99u, 99u, 99u),
    expectTrue(!fuse::renderer::froxel_util::wouldRejectDensityLookupAtCoord(grid, desc, 3u, 1u, 2u),
    expectTrue(!fuse::renderer::froxel_util::tryPreflightStrictDensityLookupAtIndex(emptyGrid, desc, 0u, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::tryShouldSkipFroxelPopulate(desc, camera, params, populateReason),
               "tryShouldSkipFroxelPopulate false for valid populate inputs");
    expectTrue(fuse::renderer::froxel_util::tryShouldSkipFroxelPopulate(desc, camera, zeroDensity, populateReason),
               "tryShouldSkipFroxelPopulate true for zero density");
    expectTrue(fuse::renderer::froxel_util::tryShouldSkipFroxelPopulate(zeroDesc, camera, params, populateReason),
               "tryShouldSkipFroxelPopulate true for empty desc");

// --- deepen additive from deepen-froxel-volumetrics-b511-8b99 ---
void testFroxelTrilinearSampleAndPopulatePreflightGuards() {
               "tryPreflightTrilinearSample succeeds on accessible grid");
               "tryCanTrilinearSampleAtCoords rejects empty desc");
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason), "empty_grid") == 0,
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::HardOutOfBounds,
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(emptyGrid, desc, inBounds, rejectedTrilinear, trilinearReason),
               "tryValidateSampleCoords succeeds for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::tryPreflightDensityLookupAtIndex(grid, desc, 0u, lookupReason),
               "tryPreflightDensityLookupAtIndex succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::tryPreflightDensityLookupAtCoord(grid, desc, 1u, 1u, 2u, lookupReason),
               "tryPreflightDensityLookupAtCoord succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::wouldRejectDensityLookupAtCoord(grid, desc, 1u, 1u, 2u),
    expectTrue(fuse::renderer::froxel_util::wouldRejectDensityLookupAtIndex(emptyGrid, desc, 0u),
    expectTrue(fuse::renderer::froxel_util::wouldRejectDensityLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u),
    expectTrue(!fuse::renderer::froxel_util::tryPreflightDensityLookupAtIndex(emptyGrid, desc, 0u, lookupReason),
               "tryPreflightDensityLookupAtIndex rejects empty storage");
    expectTrue(fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(desc, camera, params, &populateReason),
               "preflightPopulateFromAnalyticFog succeeds for valid inputs");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(desc, camera, zeroDensity, &populateReason),
               "preflightPopulateFromAnalyticFog rejects zero density");
    expectTrue(fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(desc, camera, params),
               "preflightPopulateFromAnalyticFog without reason output succeeds for valid inputs");
    testFroxelTrilinearSampleAndPopulatePreflightGuards();

// --- deepen additive from deepen-b511-froxel-guards-f8af ---
void testFroxelTrilinearAndPreflightGuards() {
    expectTrue(fuse::renderer::froxel_util::preflightSampleAtCoords(grid, desc, inBounds, sampleReason),
               "preflightSampleAtCoords succeeds for in-bounds coords");
               "preflightSampleAtCoords reports no reject reason for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightSampleAtCoords(grid, desc, inBounds),
               "preflightSampleAtCoords without reason succeeds for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, inBounds, sampleReason),
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, inBounds),
               "preflightTrilinearSample without reason succeeds for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, warnWeights, sampleReason),
               "preflightTrilinearSample warns but succeeds for clampable weights");
               "preflightTrilinearSample reports invalid_weights for clampable weights");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, inBounds, trilinearSample, sampleReason),
               "trySampleDensityTrilinear with reason succeeds for in-bounds coords");
               "trySampleDensityTrilinear with reason matches unguarded sample");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 0u, lookupReason),
               "preflightDensityLookupAtIndex succeeds for accessible grid");
               "preflightDensityLookupAtIndex reports no reject reason for in-range index");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 0u),
               "preflightDensityLookupAtIndex without reason succeeds for accessible grid");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 999u, lookupReason),
               "preflightDensityLookupAtIndex warns but succeeds for OOB index");
               "preflightDensityLookupAtIndex reports index_out_of_range for OOB index");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 3u, 1u, 2u, lookupReason),
               "preflightDensityLookupAtCoord succeeds for in-range coords");
               "preflightDensityLookupAtCoord reports no reject reason for in-range coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 99u, 99u, 99u, lookupReason),
               "preflightDensityLookupAtCoord warns but succeeds for OOB coords");
               "trySampleDensityAtScreen with dual reasons succeeds on accessible grid");
               "trySampleDensityAtScreen with dual reasons matches unguarded screen sample");
               "preflightPopulateFromAnalyticFog reports no reject reason for valid inputs");
               "preflightPopulateFromAnalyticFog without reason succeeds for valid inputs");
               "preflightPopulateFromAnalyticFog rejects invalid camera");
               "preflightPopulateFromAnalyticFog reports invalid_camera reject reason");
    testFroxelTrilinearAndPreflightGuards();

// --- deepen additive from deepen-b511-froxel-preflights-49c6 ---
void testFroxelTrilinearPreflightAndSkipGuards() {
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookup(grid, mismatched),
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookupAtCoord(grid, mismatched, 0u, 0u, 0u),
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, mismatched, 0u, 0u, 0u, lookupReason),
               "tryCanLookupAtCoord rejects desc mismatch");
    expectTrue(!fuse::renderer::FroxelGridLayout::wouldSkipSampleCoords(inBounds, desc),
    expectTrue(fuse::renderer::FroxelGridLayout::wouldSkipSampleCoords(hardOob, desc),
               "canPreflightTrilinearSample succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipTrilinearSample(grid, desc, inBounds),
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::InvalidWeights,
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason), "invalid_weights") ==
    expectTrue(!fuse::renderer::froxel_util::wouldSkipTrilinearSample(grid, desc, warnWeights),
               "tryPreflightTrilinearSample rejects empty storage");
    expectTrue(fuse::renderer::froxel_util::wouldSkipTrilinearSample(emptyGrid, desc, inBounds),
    expectTrue(!fuse::renderer::froxel_util::tryPreflightTrilinearSample(grid, mismatched, inBounds, trilinearReason),
               "tryPreflightTrilinearSample rejects desc mismatch");
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason), "desc_mismatch") ==
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::CoordsOutOfRange,
    expectTrue(fuse::renderer::froxel_util::wouldSkipTrilinearSample(grid, desc, hardOob),
    expectTrue(!fuse::renderer::froxel_util::tryPreflightTrilinearSample(grid, zeroDesc, inBounds, trilinearReason),
               "tryPreflightTrilinearSample rejects empty desc");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, hardOob, rejectedOob,
    expectTrue(!fuse::renderer::froxel_util::wouldSkipAnalyticPopulate(desc, camera, params),
    expectTrue(fuse::renderer::froxel_util::wouldSkipAnalyticPopulate(zeroDesc, camera, params),
    expectTrue(fuse::renderer::froxel_util::wouldSkipAnalyticPopulate(desc, badCamera, params),
    expectTrue(fuse::renderer::froxel_util::wouldSkipAnalyticPopulate(desc, camera, zeroDensity),
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(corrupt, desc, densityReason),
    testFroxelTrilinearPreflightAndSkipGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-2131 ---
void testFroxelTrilinearAndPopulateDeepGuards() {
    fuse::renderer::DensityTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::DensityTrilinearSampleRejectReason::None;
    expectTrue(trilinearReason == fuse::renderer::DensityTrilinearSampleRejectReason::None,
    expectTrue(std::strcmp(fuse::renderer::densityTrilinearSampleRejectReasonLabel(trilinearReason), "none") == 0,
               "tryCanTrilinearSampleAtCoords succeeds when weights will clamp");
    expectTrue(trilinearReason == fuse::renderer::DensityTrilinearSampleRejectReason::ClampableWeights,
    expectTrue(std::strcmp(fuse::renderer::densityTrilinearSampleRejectReasonLabel(trilinearReason),
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, warnWeights, trilinearSample,
               "trySampleDensityTrilinear with trilinear reason succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::DensityTrilinearSampleRejectReason::InvalidSampleCoords,
    expectTrue(trilinearReason == fuse::renderer::DensityTrilinearSampleRejectReason::EmptyGrid,
    expectTrue(trilinearReason == fuse::renderer::DensityTrilinearSampleRejectReason::EmptyStorage,
    expectTrue(!fuse::renderer::froxel_util::tryCanTrilinearSampleAtCoords(grid, mismatched, inBounds, trilinearReason),
               "tryCanTrilinearSampleAtCoords rejects desc mismatch");
    expectTrue(trilinearReason == fuse::renderer::DensityTrilinearSampleRejectReason::DescMismatch,

// --- deepen additive from deepen-froxel-volumetrics-b511-e35c ---
void testFroxelTrilinearAndPreflightDeepenGuards() {
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc),
               "preflightDensityLookup succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookup(grid, fuse::renderer::FroxelGridDesc{}),
               "preflightDensityLookup rejects empty desc");
               "tryPreflightDensityLookup reports no reject reason");
               "tryPreflightDensityLookup warns on OOB index");
               "tryPreflightDensityLookup reports index_out_of_range");
               "tryPreflightDensityLookup reports empty_storage");
    fuse::renderer::TrilinearSampleRejectReason trilinearReason = fuse::renderer::TrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightTrilinearSampleCoords(inBounds, desc, trilinearReason),
               "tryPreflightTrilinearSampleCoords succeeds for in-bounds coords");
    expectTrue(trilinearReason == fuse::renderer::TrilinearSampleRejectReason::None,
    expectTrue(std::strcmp(fuse::renderer::trilinearSampleRejectReasonLabel(trilinearReason), "none") == 0,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightTrilinearSampleCoords(reversedSlice, desc, trilinearReason),
               "tryPreflightTrilinearSampleCoords rejects reversed slice corners");
    expectTrue(trilinearReason == fuse::renderer::TrilinearSampleRejectReason::InvalidSampleCoords,
    expectTrue(std::strcmp(fuse::renderer::trilinearSampleRejectReasonLabel(trilinearReason), "invalid_sample_coords") == 0,
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightTrilinearSampleCoords(warnTz, desc, trilinearReason),
               "tryPreflightTrilinearSampleCoords warns but succeeds for clampable tz");
    expectTrue(trilinearReason == fuse::renderer::TrilinearSampleRejectReason::InvalidWeights,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightTrilinearSampleCoords(hardOob, desc, trilinearReason),
               "tryPreflightTrilinearSampleCoords rejects hard OOB tile coord");
    expectTrue(trilinearReason == fuse::renderer::TrilinearSampleRejectReason::OutOfBounds,
    expectTrue(trilinearReason == fuse::renderer::TrilinearSampleRejectReason::EmptyStorage,
    expectTrue(fuse::renderer::froxel_util::tryPreflightPopulate(desc, camera, params, emptyGrid, populateReason),
               "tryPreflightPopulate succeeds for valid inputs with empty output grid");
    expectTrue(fuse::renderer::froxel_util::tryPreflightPopulate(mismatched, camera, params, grid, populateReason),
               "tryPreflightPopulate still succeeds when output grid will reallocate");
    expectTrue(!fuse::renderer::froxel_util::tryPreflightPopulate(desc, camera, zeroDensity, emptyGrid, populateReason),
               "tryPreflightPopulate rejects zero density");
    testFroxelTrilinearAndPreflightDeepenGuards();

// --- deepen additive from deepen-b511-froxel-guards-c9a6 ---
               "tryCanSampleDensityTrilinear succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::tryCanSampleDensityTrilinear(grid, desc, warnWeights, trilinearReason),
               "tryCanSampleDensityTrilinear warns but succeeds for clampable weights");
               "tryCanSampleDensityTrilinear rejects hard OOB tile coord");
               "tryCanSampleDensityTrilinear rejects empty froxel desc");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookupAtIndex(grid, desc, 0u),
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookupAtIndex(emptyGrid, desc, 0u),
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(clampable, desc, clampReason),
               "tryClampSampleCoords with reason produces in-bounds coords");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(emptyClamp, zeroDesc, clampReason),
               "tryClampSampleCoords with reason rejects empty desc");
    expectTrue(clampReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "valid populate inputs do not skip via tryShouldSkip");
               "zero density skips via tryShouldSkip");
    expectTrue(fuse::renderer::froxel_util::tryShouldSkipFroxelPopulate(desc, badCamera, params, populateReason),
               "invalid camera skips via tryShouldSkip");

// --- deepen additive from deepen-b511-froxel-guards-5875 ---
    expectTrue(fuse::renderer::classifyDensityLookupReject(grid, desc, 0u) ==
               "classifyDensityLookupReject returns none for in-range index");
    expectTrue(fuse::renderer::classifyDensityLookupReject(grid, desc, 999u) ==
               "classifyDensityLookupReject returns index_out_of_range for OOB index");
    expectTrue(fuse::renderer::classifyDensityLookupReject(emptyGrid, desc, 0u) ==
               "classifyDensityLookupReject returns empty_storage for empty grid");
    expectTrue(fuse::renderer::classifyFroxelTrilinearSampleReject(grid, desc, inBounds) ==
               "classifyFroxelTrilinearSampleReject returns none for in-bounds coords");
               "classifySampleCoordReject returns invalid_weights for clampable weights");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordReject(hardOob, desc) ==
               "classifySampleCoordReject returns out_of_bounds for hard OOB coords");
               "classifyFroxelPopulateReject returns none for valid inputs");
    expectTrue(fuse::renderer::classifyFroxelPopulateReject(zeroDesc, camera, params) ==
               "classifyFroxelPopulateReject returns empty_desc for zero-dimension grid");
    expectTrue(fuse::renderer::classifyFroxelPopulateReject(desc, badCamera, params) ==
               "classifyFroxelPopulateReject returns invalid_camera for bad camera");

// --- deepen additive from deepen-froxel-volumetrics-b511-4fcc ---
void testFroxelTrilinearAndPopulatePreflightGuards() {
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleDensityTrilinear(undersized, desc, inBounds, trilinearReason),
               "tryCanSampleDensityTrilinear rejects undersized storage");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleDensityTrilinear(grid, mismatched, inBounds, trilinearReason),
               "tryCanSampleDensityTrilinear rejects desc mismatch");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::NotSampleable,
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason), "not_sampleable") == 0,
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleDensityTrilinear(grid, desc, reversed, trilinearReason),
               "tryCanSampleDensityTrilinear rejects unordered corners");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(reversed, desc, sampleReason),
               "tryPreflightSampleCoords rejects unordered corners");
               "tryCanSampleDensityTrilinear still succeeds when weights will be clamped");
    expectTrue(fuse::renderer::froxel_util::canPreflightPopulateFromAnalyticFog(desc, camera, params),
               "canPreflightPopulateFromAnalyticFog succeeds for valid inputs");
    expectTrue(fuse::renderer::froxel_util::tryPreflightPopulateFromAnalyticFog(desc, camera, params, populateReason),
               "tryPreflightPopulateFromAnalyticFog succeeds for valid inputs");
    expectTrue(!fuse::renderer::froxel_util::tryPreflightPopulateFromAnalyticFog(desc, camera, zeroDensity, populateReason),
               "tryPreflightPopulateFromAnalyticFog rejects zero density");
    testFroxelTrilinearAndPopulatePreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-a69f ---
    expectTrue(fuse::renderer::froxel_util::tryCanLookupForDensitySample(grid, desc, lookupReason),
               "tryCanLookupForDensitySample succeeds on accessible grid");
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupForDensitySample(emptyGrid, desc, lookupReason),
               "tryCanLookupForDensitySample rejects empty storage");
    expectTrue(!fuse::renderer::froxel_util::wouldRejectSampleCoords(inBounds, desc),
    expectTrue(fuse::renderer::froxel_util::wouldRejectSampleCoords(hardOob, desc),
               "tryCanTrilinearSampleAtCoords still succeeds when weights will be clamped");
    expectTrue(fuse::renderer::froxel_util::tryPreflightPopulateAllocation(desc, populateReason),
               "tryPreflightPopulateAllocation succeeds for non-empty desc");
    expectTrue(!fuse::renderer::froxel_util::tryPreflightPopulateAllocation(zeroDesc, populateReason),
               "tryPreflightPopulateAllocation rejects empty desc");

// --- deepen additive from deepen-froxel-b511-guards-2eab ---
               "tryCanSampleTrilinear succeeds for in-bounds coords");
               "tryCanSampleTrilinear succeeds when weights will be clamped");
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason), "clampable_weights") ==
               "trySampleDensityTrilinear with trilinear reason succeeds");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::GridInaccessible,
               "preflightDensityLookupAtIndex warns on OOB index");
               "preflightDensityLookupAtCoord warns on OOB coords");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u, lookupReason),
               "preflightDensityLookupAtCoord rejects empty storage");
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(clampable, desc, sampleReason),
               "tryClampSampleCoords with reason succeeds for clampable weights");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(desc, camera, zeroDensity, populateReason),

// --- deepen additive from deepen-froxel-volumetrics-b511-a361 ---
void testFroxelClassifyAndPreflightGuards() {
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 5u, &lookupReason),
    expectTrue(fuse::renderer::froxel_util::tryPreflightDensityLookupAtIndex(grid, desc, 999u, lookupReason),
               "tryPreflight density lookup at index succeeds with clamp warning");
               "tryPreflight density lookup at index reports index_out_of_range");
    expectTrue(fuse::renderer::classifyDensityLookupCoordReject(grid, desc, 1u, 1u, 2u) ==
    expectTrue(fuse::renderer::classifyDensityLookupCoordReject(grid, desc, 99u, 99u, 99u) ==
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 1u, 1u, 2u, &lookupReason),
    expectTrue(fuse::renderer::froxel_util::classifyTrilinearSampleReject(grid, desc, inBounds) ==
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, inBounds, &sampleReason),
    expectTrue(fuse::renderer::froxel_util::tryPreflightTrilinearSample(grid, desc, warnWeights, sampleReason),
               "tryPreflight trilinear sample succeeds with clampable weights");
               "tryPreflight trilinear sample reports invalid_weights");
               "trySampleDensityTrilinear with reason succeeds on accessible grid");
    expectTrue(fuse::renderer::preflightFroxelPopulate(desc, camera, params, &populateReason),
    expectTrue(fuse::renderer::tryPreflightFroxelPopulate(desc, camera, params, populateReason),
               "tryPreflight populate succeeds for valid inputs");
    expectTrue(!fuse::renderer::preflightFroxelPopulate(desc, camera, zeroDensity, &populateReason),
    expectTrue(fuse::renderer::classifyScreenMappingReject(0.5f, 0.5f, 10.f, desc, camera) ==
    expectTrue(fuse::renderer::preflightScreenDepthMapping(0.5f, 0.5f, 10.f, desc, camera, &mapReason),
    expectTrue(fuse::renderer::classifyScreenMappingReject(0.5f, 0.5f, 0.01f, desc, camera) ==
    expectTrue(!fuse::renderer::preflightScreenDepthMapping(0.5f, 0.5f, 0.01f, desc, camera, &mapReason),
    expectTrue(fuse::renderer::classifyScreenMappingReject(0.5f, 0.5f, 10.f, zeroDesc, camera) ==
    testFroxelClassifyAndPreflightGuards();

// --- deepen additive from deepen-froxel-volumetric-guards-b511-453e ---
void testFroxelTrilinearAndDensityLookupGuards() {
    fuse::renderer::SampleCoordRejectReason validateReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(inBounds, desc, validateReason),
    expectTrue(validateReason == fuse::renderer::SampleCoordRejectReason::None,
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(invalidWeights, desc, validateReason),
               "tryValidateSampleCoords rejects invalid interpolation weights");
    expectTrue(validateReason == fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "tryPreflightTrilinearSample warns but succeeds for clampable slice weight");
               "tryPreflightTrilinearSample rejects hard OOB tile coord");
    expectTrue(fuse::renderer::froxel_util::tryLookupDensityFromScreen(
               "tryLookupDensityFromScreen succeeds on accessible grid");
               "tryLookupDensityFromScreen matches unguarded screen sample");
    expectTrue(!fuse::renderer::froxel_util::tryLookupDensityFromScreen(
               "tryLookupDensityFromScreen rejects empty storage");
               "tryLookupDensityFromScreen rejects depth below near plane");

// --- deepen additive from deepen-b511-froxel-guards-13b8 ---
void testFroxelTrilinearStrictLookupAndPopulateGuards() {
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndexStrict(grid, desc, 5u, lookupReason),
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndexStrict(grid, desc, 999u, lookupReason),
    expectTrue(fuse::renderer::froxel_util::canPreflightTrilinearDensitySample(grid, desc, inBounds),
    expectTrue(fuse::renderer::froxel_util::canPreflightTrilinearDensitySample(grid, desc, warnWeights),
    expectTrue(!fuse::renderer::froxel_util::canPreflightTrilinearDensitySample(grid, desc, hardOob),
    expectTrue(fuse::renderer::froxel_util::tryPreflightTrilinearDensitySample(grid, desc, inBounds, sampleReason),
               "tryPreflightTrilinearDensitySample succeeds for in-bounds coords");
               "trySampleDensityTrilinear with reason rejects hard OOB coords");
    expectNear(rejectedTrilinear, 0.f, 1e-6f, "trySampleDensityTrilinear with reason zeroes output on hard OOB rejection");
    expectTrue(fuse::renderer::froxel_util::tryValidatePopulateResult(populated, desc, camera, params, populateReason),
    expectTrue(fuse::renderer::froxel_util::tryValidatePopulateResult(skipped, desc, camera, zeroDensity, populateReason),
    expectTrue(!fuse::renderer::froxel_util::tryValidatePopulateResult(mismatched, desc, camera, params, populateReason),

// --- deepen additive from deepen-b511-froxel-preflights-e22d ---
void testFroxelPreflightHelperGuards() {
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 0u, &lookupReason),
               "preflightDensityLookupAtIndex succeeds on accessible grid");
               "preflightDensityLookupAtIndex reports no reject reason");
               "preflightDensityLookupAtIndex succeeds without reason output");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 999u, &lookupReason),
               "preflightDensityLookupAtIndex reports index_out_of_range");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 3u, 1u, 2u, &lookupReason),
               "preflightDensityLookupAtCoord reports no reject reason");
               "preflightDensityLookupAtCoord reports index_out_of_range");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookupAtIndex(emptyGrid, desc, 0u, &lookupReason),
               "preflightDensityLookupAtIndex rejects empty storage");
               "preflightDensityLookupAtIndex reports empty_storage");
               "preflightSampleCoords reports no reject reason");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(inBounds, desc),
               "preflightSampleCoords succeeds without reason output");
    expectTrue(fuse::renderer::froxel_util::preflightSampleAtCoords(grid, desc, inBounds, &sampleReason),
               "preflightSampleAtCoords succeeds on accessible grid");
               "preflightSampleAtCoords reports no reject reason");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, warnWeights, &sampleReason),
               "preflightTrilinearSample reports invalid_weights");
               "tryPreflightTrilinearSample matches preflightTrilinearSample");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, hardOob, &sampleReason),
               "preflightTrilinearSample rejects hard OOB coords");
               "preflightTrilinearSample reports out_of_bounds");
    expectTrue(!fuse::renderer::froxel_util::tryPreflightTrilinearSample(grid, desc, hardOob, sampleReason),
               "trySampleDensityTrilinear succeeds after preflight passes");
               "trySampleDensityTrilinear matches unguarded sample after preflight");
               "preflightPopulateFromAnalyticFog reports no reject reason");
               "preflightPopulateFromAnalyticFog succeeds without reason output");
               "tryPreflightPopulateFromAnalyticFog matches preflightPopulateFromAnalyticFog");
               "preflightPopulateFromAnalyticFog reports zero_density");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(zeroDesc, camera, params, &populateReason),
               "preflightPopulateFromAnalyticFog rejects empty desc");
               "preflightPopulateFromAnalyticFog reports empty_desc");
    testFroxelPreflightHelperGuards();

// --- deepen additive from deepen-b511-froxel-guards-ca59 ---
void testFroxelTrilinearSkipAndWouldSkipGuards() {
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookupAtIndex(grid, desc, 999u),
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookupAtCoord(grid, desc, 0u, 0u, 0u),
    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelTrilinearSample(grid, desc, inBounds),
    expectTrue(fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, warnWeights, trilinearReason),
               "tryCanSampleTrilinearAtCoords succeeds when weights will be clamped");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, hardOob, trilinearReason),
               "tryCanSampleTrilinearAtCoords rejects hard OOB tile coord");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelTrilinearSample(grid, desc, hardOob),
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(grid, zeroDesc, inBounds, trilinearReason),
               "tryCanSampleTrilinearAtCoords rejects empty froxel desc");
               "valid populate inputs do not skip via wouldSkipFroxelPopulate");
               "empty desc skips via wouldSkipFroxelPopulate");
               "zero density skips via wouldSkipFroxelPopulate");
               "invalid camera skips via wouldSkipFroxelPopulate");

// --- deepen additive from deepen-b511-froxel-guards-1123 ---
void testFroxelClassifyBlockingAndPreflightGuards() {
               "classifyScreenMappingReject none for valid inputs");
                   fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange),
               "classifyScreenMappingReject empty_grid for zero desc");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordsReject(inBounds, desc) ==
               "classifySampleCoordsReject none for valid coords");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordsReject(warnWeights, desc) ==
               "classifySampleCoordsReject invalid_weights for OOB interpolation");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordsReject(hardOob, desc) ==
               "classifySampleCoordsReject out_of_bounds for hard OOB tile");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenDepthToSampleCoords(
               "preflightScreenDepthToSampleCoords succeeds for valid inputs");
               "preflightScreenDepthToSampleCoords reports no reject reason");
               "tryClampSampleCoords with reason reports no reject reason");
    expectTrue(fuse::renderer::froxel_util::preflightGridDensity(grid, desc),
               "classifyGridDensityReject undersized_storage for short buffer");
               "preflightGridDensity reports undersized reject reason");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupRejectAtIndex(grid, desc, 0u) ==
               "classifyDensityLookupRejectAtIndex none for in-range index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupRejectAtIndex(grid, desc, 999u) ==
               "classifyDensityLookupRejectAtIndex index_out_of_range for OOB index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupRejectAtCoord(emptyGrid, desc, 0u, 0u, 0u) ==
               "classifyDensityLookupRejectAtCoord empty_storage for empty grid");
               "classifyFroxelTrilinearSampleReject none for valid coords");
                   fuse::renderer::FroxelTrilinearSampleRejectReason::None),
    expectTrue(fuse::renderer::froxel_util::classifyFroxelTrilinearSampleReject(grid, desc, warnWeights) ==
               "classifyFroxelTrilinearSampleReject clampable_weights for OOB weights");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelTrilinearSampleReject(grid, desc, hardOob) ==
               "classifyFroxelTrilinearSampleReject invalid_sample_coords for hard OOB");
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtTrilinear(grid, desc, inBounds, trilinearReason),
               "tryCanSampleAtTrilinear succeeds for valid coords");
               "tryCanSampleAtTrilinear reports no reject reason");
    fuse::f32 preflightDensity = 0.f;
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSample(
                   grid, desc, inBounds, &preflightDensity, &trilinearReason),
               "preflightFroxelTrilinearSample succeeds for valid coords");
    expectNear(preflightDensity,
               "preflightFroxelTrilinearSample matches unguarded trilinear sample");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelTrilinearSample(
                   emptyGrid, desc, inBounds, &preflightDensity, &trilinearReason),
               "preflightFroxelTrilinearSample rejects inaccessible grid");
               "preflightFroxelTrilinearSample reports inaccessible_grid reject reason");
               "classifyFroxelPopulateReject none for valid populate inputs");
    testFroxelClassifyBlockingAndPreflightGuards();

// --- deepen additive from deepen-froxel-preflight-guards-cfa4 ---
void testFroxelPreflightDeepenGuards() {
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupIndexReject(grid, desc, 3u) ==
               "classifyDensityLookupIndexReject none for valid index");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 3u),
               "preflightDensityLookupAtIndex succeeds for valid index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupIndexReject(grid, desc, 999u) ==
               "classifyDensityLookupIndexReject index_out_of_range for OOB index");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityLookupAtIndex(grid, desc, 3u),
               "wouldSkipDensityLookupAtIndex false for valid index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupCoordReject(grid, desc, 1u, 1u, 2u) ==
               "classifyDensityLookupCoordReject none for valid coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 1u, 1u, 2u),
               "preflightDensityLookupAtCoord succeeds for valid coords");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupCoordReject(grid, desc, 99u, 99u, 99u) ==
               "classifyDensityLookupCoordReject index_out_of_range for OOB coords");
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookupAtCoord(grid, desc, 99u, 99u, 99u) == false,
               "wouldSkipDensityLookupAtCoord false when only clamp warning applies");
               "classifySampleCoordReject none for valid coords");
               "preflightSampleCoords succeeds for valid coords");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(warnWeights, desc),
               "classifySampleCoordReject out_of_bounds for hard OOB indices");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightSampleCoords(hardOob, desc),
               "preflightSampleCoords rejects hard OOB indices");
    expectTrue(fuse::renderer::froxel_util::classifyDensitySampleCoordReject(grid, desc, inBounds) ==
               "classifyDensitySampleCoordReject none for valid coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensitySampleAtCoords(grid, desc, inBounds),
               "preflightDensitySampleAtCoords succeeds for valid coords");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearDensitySample(grid, desc, inBounds),
               "preflightTrilinearDensitySample succeeds for valid coords");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipTrilinearDensitySample(grid, desc, inBounds),
               "wouldSkipTrilinearDensitySample false for valid coords");
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensitySampleAtCoords(grid, desc, hardOob),
               "wouldSkipDensitySampleAtCoords true for hard OOB coords");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenMappingReject(0.25f, 0.25f, 10.f, desc, camera) ==
               "classifyScreenMappingReject none for valid screen depth");
               "preflightScreenDepthToSampleCoords succeeds for valid screen depth");
    expectTrue(!fuse::renderer::screenMappingRejectReasonIsBlocking(mapReason),
    expectTrue(!fuse::renderer::FroxelGridLayout::wouldSkipScreenDepthMapping(0.25f, 0.25f, 10.f, desc, camera),
               "wouldSkipScreenDepthMapping false for valid screen depth");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenDepthToFroxelIndex(
               "preflightScreenDepthToFroxelIndex succeeds for valid screen depth");
               "preflightScreenDepthToFroxelIndex matches sample-coord preflight index");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenMappingReject(1.5f, 0.25f, 10.f, desc, camera) ==
                   fuse::renderer::ScreenMappingRejectReason::ScreenCoordsOutOfRange,
               "classifyScreenMappingReject screen_coords_out_of_range for OOB screen X");
               "preflightScreenDepthToSampleCoords succeeds when screen coords will be clamped");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::ScreenCoordsOutOfRange,
                   fuse::renderer::ScreenMappingRejectReason::NonFiniteDepth,
               "classifyScreenMappingReject non_finite_depth for NaN depth");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenDepthToSampleCoords(
               "preflightScreenDepthToSampleCoords rejects NaN depth");
    expectTrue(fuse::renderer::screenMappingRejectReasonIsBlocking(mapReason),
    expectTrue(fuse::renderer::froxel_util::preflightScreenDensitySample(grid, desc, camera, 0.25f, 0.25f, 10.f),
               "preflightScreenDensitySample succeeds for valid screen depth");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipScreenDensitySample(grid, desc, camera, 0.25f, 0.25f, 10.f),
               "wouldSkipScreenDensitySample false for valid screen depth");
    expectTrue(fuse::renderer::froxel_util::wouldSkipScreenDensitySample(
               "wouldSkipScreenDensitySample true for below-near depth");
               "classifyGridDensityReject none for valid grid");
               "preflightGridDensity succeeds for valid grid");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipGridDensityValidation(grid, desc),
               "wouldSkipGridDensityValidation false for valid grid");
               "classifyGridDensityReject undersized_storage for short vector");
    expectTrue(fuse::renderer::froxel_util::wouldSkipGridDensityValidation(undersized, desc),
               "wouldSkipGridDensityValidation true for undersized storage");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipPopulateFromAnalyticFog(desc, camera, params),
               "wouldSkipPopulateFromAnalyticFog false for valid inputs");
    expectTrue(fuse::renderer::froxel_util::wouldSkipPopulateFromAnalyticFog(desc, camera, zeroDensity),
               "wouldSkipPopulateFromAnalyticFog true for zero density");
    testFroxelPreflightDeepenGuards();

// --- deepen additive from deepen-b511-froxel-guards-700f ---
void testFroxelClassifyPreflightAndWouldSkipGuards() {
    expectTrue(!fuse::renderer::gridDensityRejectReasonIsBlocking(fuse::renderer::GridDensityRejectReason::EmptyDesc),
                   fuse::renderer::FroxelTrilinearSampleRejectReason::InaccessibleGrid),
               "classifySampleCoordReject reports none for in-bounds coords");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenDepthMapping(
               "preflightScreenDepthMapping succeeds in range");
               "classifyScreenMappingReject reports none in range");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenDepthMapping(
               "preflightScreenDepthMapping rejects depth below near plane");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 5u, &lookupReason),
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(grid, desc, 999u) ==
               "classifyDensityLookupReject reports index_out_of_range for OOB index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupRejectAtCoord(grid, desc, 3u, 1u, 2u) ==
               "classifyDensityLookupRejectAtCoord reports none for last valid coords");
               "preflightGridDensity succeeds on accessible grid");
               "classifyGridDensityReject reports undersized_storage");
    expectTrue(fuse::renderer::froxel_util::tryCanTrilinearSample(grid, desc, inBounds, trilinearReason),
               "tryCanTrilinearSample succeeds on accessible grid");
               "preflightTrilinearSample succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::classifyTrilinearSampleReject(emptyGrid, desc, inBounds) ==
               "classifyTrilinearSampleReject reports inaccessible_grid for empty storage");
    expectTrue(!fuse::renderer::froxel_util::tryCanTrilinearSample(emptyGrid, desc, hardOob, trilinearReason),
               "tryCanTrilinearSample rejects inaccessible grid");
               "preflightScreenDensitySample succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::classifyScreenDensitySampleReject(
               "classifyScreenDensitySampleReject reports depth_out_of_range below near plane");
               "classifyFroxelPopulateReject reports zero_density");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelGrid(zeroDesc),
               "wouldSkipFroxelGrid true for empty desc");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelLookup(emptyGrid, desc),
               "wouldSkipFroxelLookup true for empty storage");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelMarch(zeroMarchGrid, desc),
               "wouldSkipFroxelMarch true for uniformly zero grid");
    testFroxelClassifyPreflightAndWouldSkipGuards();

// --- deepen additive from deepen-b511-froxel-guards-9fe1 ---
void testFroxelGridDensityPreflightTrilinearAndPopulateGuards() {
    expectTrue(fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, inBounds, sampleReason),
    expectTrue(fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, warnWeights, sampleReason),
               "tryCanSampleTrilinearAtCoords warns but succeeds for clampable weights");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleTrilinearAtCoords(grid, desc, hardOob, sampleReason),
    testFroxelGridDensityPreflightTrilinearAndPopulateGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-2f19 ---
               "classifySampleCoordReject out_of_bounds for hard OOB tile");
    fuse::renderer::FroxelSampleCoords preflightCoords{};
                   0.5f, 0.5f, 10.f, desc, camera, &preflightCoords),
               "preflightScreenDepthToSampleCoords succeeds in range");
    expectTrue(preflightCoords.tileX0 < desc.tilesX && preflightCoords.sliceZ0 < desc.slicesZ,
               "preflightScreenDepthToSampleCoords returns mapped coords");
    fuse::u32 preflightIndex = 0u;
                   0.5f, 0.5f, 10.f, desc, camera, &preflightIndex, &mapReason),
               "preflightScreenDepthToFroxelIndex succeeds in range");
    expectTrue(preflightIndex < desc.froxelCount(), "preflightScreenDepthToFroxelIndex returns valid index");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenMappingReject(0.5f, 0.5f, 0.01f, desc, camera) ==
               "classifyScreenMappingReject depth_out_of_range below near plane");
               "classifyGridDensityReject undersized_storage");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 999u),
               "preflightDensityLookupAtIndex still succeeds with clamp warning");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u),
               "classifyFroxelPopulateReject zero_density");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearDensitySample(grid, desc, inBounds, &sampleReason),
               "preflightTrilinearDensitySample succeeds for accessible grid");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearDensitySample(emptyGrid, desc, inBounds, &sampleReason),
               "preflightTrilinearDensitySample rejects empty storage");
    expectTrue(fuse::renderer::froxel_util::preflightDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               "preflightDensityAtScreen succeeds for accessible grid");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityAtScreen(
               "preflightDensityAtScreen rejects empty storage");
               "preflightDensityAtScreen rejects depth below near plane");

// --- deepen additive from deepen-froxel-b511-guards-2df3 ---
void testFroxelVolumetricDeepenGuards() {
    expectTrue(fuse::renderer::FroxelGridLayout::preflightFroxelSampleCoords(inBounds, desc),
               "preflightFroxelSampleCoords succeeds for in-bounds coords");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightFroxelSampleCoords(warnWeights, desc, &sampleReason),
               "preflightFroxelSampleCoords warns but succeeds for clampable weights");
               "preflightFroxelSampleCoords reports invalid_weights for clampable weights");
    expectTrue(fuse::renderer::tryValidateFroxelCamera(camera, cameraReason),
               "tryValidateFroxelCamera succeeds for valid camera");
    expectTrue(!fuse::renderer::tryValidateFroxelCamera(badPlanes, cameraReason),
               "tryValidateFroxelCamera rejects invalid planes");
    expectTrue(cameraReason == fuse::renderer::FroxelCameraRejectReason::InvalidPlanes,
    expectTrue(std::strcmp(fuse::renderer::froxelCameraRejectReasonLabel(cameraReason), "invalid_planes") == 0,
    expectTrue(!fuse::renderer::tryValidateFroxelCamera(zeroScreen, cameraReason),
               "tryValidateFroxelCamera rejects zero screen width");
    expectTrue(cameraReason == fuse::renderer::FroxelCameraRejectReason::ZeroScreenDimensions,
    expectTrue(fuse::renderer::froxelCameraRejectReasonIsBlocking(cameraReason),
               "classifyScreenMappingReject none for in-range depth");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightFroxelScreenDepth(0.5f, 0.5f, 10.f, desc, camera),
               "preflightFroxelScreenDepth succeeds for in-range depth");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightFroxelScreenDepth(0.5f, 0.5f, 0.01f, desc, camera),
               "preflightFroxelScreenDepth rejects depth below near plane");
    expectTrue(fuse::renderer::screenMappingRejectReasonIsBlocking(mapReason) == false,
               "classifyDensityLookupReject none for accessible grid");
               "classifyDensityLookupReject index_out_of_range for OOB index");
               "preflightDensityLookupAtIndex still succeeds with OOB clamp warning");
    expectTrue(!fuse::renderer::densityLookupRejectReasonIsBlocking(lookupReason),
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookupAtIndex(emptyGrid, desc, 0u),
               "classifyFroxelTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSample(grid, desc, inBounds, &trilinearReason),
               "preflightFroxelTrilinearSample succeeds for valid sample");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelTrilinearSample(emptyGrid, desc, inBounds, &trilinearReason),
               "preflightFroxelTrilinearSample rejects empty storage");
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason), "empty_storage") ==
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelTrilinearSample(grid, desc, hardOob, &trilinearReason),
               "preflightFroxelTrilinearSample rejects hard OOB coords");
    expectTrue(fuse::renderer::froxel_util::classifyGridDensityReject(nonFiniteGrid, desc) ==
                   fuse::renderer::GridDensityRejectReason::NonFiniteDensity,
               "classifyGridDensityReject non_finite_density for NaN storage");
    expectTrue(!fuse::renderer::froxel_util::preflightGridDensity(nonFiniteGrid, desc),
               "preflightGridDensity rejects non-finite density");
               "preflightGridDensity succeeds for finite grid");
                   fuse::renderer::GridDensityRejectReason::NonFiniteDensity),
    expectTrue(fuse::renderer::froxel_util::classifyGridDensityReject(emptyGrid, zeroDesc) ==
               "classifyGridDensityReject empty_desc for empty froxel desc");
               "classifyFroxelPopulateReject none for valid populate");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelPopulate(desc, camera, params),
               "preflightFroxelPopulate succeeds for valid populate");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(desc, camera, zeroDensity),

// --- deepen additive from deepen-froxel-volumetrics-b511-da54 ---
               "classifySampleCoordReject invalid_weights for OOB weights");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightFroxelSampleCoords(warnWeights, desc),
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightFroxelSampleCoords(hardOob, desc),
               "preflightFroxelSampleCoords rejects hard OOB tile coord");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenDepthMappingReject(
                   0.5f, 0.5f, 10.f, desc, camera) == fuse::renderer::ScreenMappingRejectReason::None,
               "classifyScreenDepthMappingReject none for in-range depth");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenDepthMapping(0.5f, 0.5f, 10.f, desc, camera),
               "preflightScreenDepthMapping succeeds for in-range depth");
               "classifyScreenDepthMappingReject depth_out_of_range below near plane");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 0u),
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 999u),
               "preflightDensityLookup warns but succeeds for OOB index that clamps");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(grid, desc, 1u, 1u, 2u) ==
               "classifyDensityLookupReject none for in-range coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 99u, 99u, 99u),
               "preflightDensityLookupAtCoord warns but succeeds for OOB coords that clamp");
               "preflightDensityLookup reports empty_storage reject reason");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSample(grid, desc, inBounds),
               "preflightFroxelTrilinearSample succeeds for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSample(grid, desc, warnWeights),
               "preflightFroxelTrilinearSample warns but succeeds for clampable weights");
               "classifyFroxelTrilinearSampleReject invalid_sample_coords for hard OOB tile");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelTrilinearSample(grid, desc, hardOob),
               "preflightFroxelTrilinearSample rejects hard OOB tile coord");
    expectTrue(fuse::renderer::froxel_util::classifyGridDensityReject(grid, zeroDesc) ==
               "classifyGridDensityReject none for empty desc (vacuously valid)");
    expectTrue(fuse::renderer::froxel_util::preflightGridDensity(grid, zeroDesc),
               "preflightGridDensity vacuously succeeds for empty desc");
               "preflightFroxelPopulate succeeds for valid populate inputs");

// --- deepen additive from deepen-froxel-b511-guards-8718 ---
void testFroxelBilinearScreenMappingAndGridDensityGuards() {
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityBilinearSample(grid, desc, inBounds),
    fuse::renderer::FroxelBilinearSampleRejectReason bilinearReason =
        fuse::renderer::FroxelBilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanBilinearSampleAtCoords(grid, desc, inBounds, bilinearReason),
               "tryCanBilinearSampleAtCoords succeeds on accessible grid");
    expectTrue(bilinearReason == fuse::renderer::FroxelBilinearSampleRejectReason::None,
    expectTrue(std::strcmp(fuse::renderer::froxelBilinearSampleRejectReasonLabel(bilinearReason), "none") == 0,
    expectTrue(!fuse::renderer::froxelBilinearSampleRejectReasonIsBlocking(bilinearReason),
    expectTrue(fuse::renderer::froxel_util::tryCanBilinearSampleAtCoords(grid, desc, warnWeights, bilinearReason),
               "tryCanBilinearSampleAtCoords warns but succeeds for clampable weights");
    expectTrue(bilinearReason == fuse::renderer::FroxelBilinearSampleRejectReason::ClampableWeights,
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityBilinearSample(grid, desc, warnWeights),
    expectTrue(!fuse::renderer::froxel_util::tryCanBilinearSampleAtCoords(grid, desc, hardOob, bilinearReason),
               "tryCanBilinearSampleAtCoords rejects hard OOB tile coord");
    expectTrue(bilinearReason == fuse::renderer::FroxelBilinearSampleRejectReason::InvalidSampleCoords,
    expectTrue(fuse::renderer::froxelBilinearSampleRejectReasonIsBlocking(bilinearReason),
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityBilinearSample(grid, desc, hardOob),
    expectTrue(!fuse::renderer::froxel_util::tryCanBilinearSampleAtCoords(emptyGrid, desc, inBounds, bilinearReason),
               "tryCanBilinearSampleAtCoords rejects empty storage");
    expectTrue(bilinearReason == fuse::renderer::FroxelBilinearSampleRejectReason::InaccessibleGrid,
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, bilinearSample, bilinearReason),
               "trySampleDensityBilinear with bilinear reason succeeds on accessible grid");
               "trySampleDensityBilinear with bilinear reason matches unguarded sample");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, hardOob, rejectedBilinear, bilinearReason),
               "trySampleDensityBilinear with bilinear reason rejects hard OOB coords");
    expectNear(rejectedBilinear, 0.f, 1e-6f, "trySampleDensityBilinear with bilinear reason zeroes output on rejection");
    expectTrue(fuse::renderer::FroxelGridLayout::wouldSkipScreenDepthMapping(
    expectTrue(!fuse::renderer::FroxelGridLayout::wouldSkipScreenDepthToFroxelIndex(
    expectTrue(fuse::renderer::FroxelGridLayout::wouldSkipScreenDepthToFroxelIndex(
    expectTrue(fuse::renderer::gridDensityRejectReasonIsBlocking(densityReason),
    expectTrue(!fuse::renderer::froxel_util::wouldSkipGridDensityValidation(emptyGrid, zeroDesc),

// --- deepen additive from deepen-b511-froxel-classify-preflight-49bd ---
void testFroxelClassifyPreflightAndBlockingGuards() {
    expectTrue(!fuse::renderer::FroxelGridLayout::wouldSkipScreenMapping(0.5f, 0.5f, 10.f, desc, camera),
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenDepthMapping(0.5f, 0.5f, 0.01f, desc, camera),
    expectTrue(fuse::renderer::FroxelGridLayout::wouldSkipScreenMapping(0.5f, 0.5f, 0.01f, desc, camera),
               "preflightDensityLookup succeeds when OOB index would clamp");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookup(emptyGrid, desc, 0u),
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(grid, desc, 99u, 99u, 99u) ==
               "classifyDensityLookupReject index_out_of_range for OOB coords");
               "classifyTrilinearSampleReject none for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityTrilinearSample(grid, desc, inBounds),
               "preflightDensityTrilinearSample succeeds for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::classifyTrilinearSampleReject(grid, desc, warnWeights) ==
               "classifyTrilinearSampleReject clampable_weights for OOB weights");
    expectTrue(fuse::renderer::froxel_util::preflightDensityTrilinearSample(grid, desc, warnWeights),
               "preflightDensityTrilinearSample succeeds for clampable weights");
    expectTrue(fuse::renderer::froxel_util::classifyTrilinearSampleReject(grid, desc, hardOob) ==
               "classifyTrilinearSampleReject invalid_sample_coords for hard OOB tile");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityTrilinearSample(grid, desc, hardOob),
               "preflightDensityTrilinearSample rejects hard OOB tile coord");
    expectTrue(!fuse::renderer::froxel_util::preflightGridDensity(undersized, desc),
               "classifyGridDensityReject none for empty desc");
    expectTrue(fuse::renderer::froxel_util::preflightGridDensity(emptyGrid, zeroDesc),
    expectTrue(fuse::renderer::froxel_util::classifyPopulateReject(desc, camera, params) ==
               "classifyPopulateReject none for valid inputs");
    expectTrue(fuse::renderer::froxel_util::classifyPopulateReject(desc, camera, zeroDensity) ==
               "classifyPopulateReject zero_density for zero density");
    expectTrue(fuse::renderer::froxel_util::classifyPopulateReject(desc, badCamera, params) ==
               "classifyPopulateReject invalid_camera for inverted planes");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(desc, badCamera, params),
               "preflightFroxelPopulate rejects invalid camera");
                   fuse::renderer::FroxelPopulateRejectReason::InvalidCamera),
    testFroxelClassifyPreflightAndBlockingGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-2bd2 ---
               "classifySampleCoordsReject invalid_weights for clampable weights");
               "preflightSampleCoords rejects hard OOB tile");
               "classifyDensityLookupReject none for valid index");
               "preflightDensityLookup succeeds for valid index");
               "preflightDensityLookup succeeds for clampable OOB index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupAtCoordReject(grid, desc, 3u, 1u, 2u) ==
               "classifyDensityLookupAtCoordReject none for valid coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 3u, 1u, 2u),
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupAtCoordReject(grid, desc, 99u, 99u, 99u) ==
               "classifyDensityLookupAtCoordReject index_out_of_range for OOB coords");
               "preflightFroxelTrilinearSample succeeds for clampable weights");
               "preflightFroxelTrilinearSample rejects hard OOB tile");

// --- deepen additive from deepen-froxel-volumetrics-b511-4d56 ---
void testFroxelPreflightClassifyAndIsBlockingGuards() {
               "classifySampleCoordReject out_of_bounds for hard OOB tile coord");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMapping(0.5f, 0.5f, 10.f, desc, camera),
               "preflightScreenMapping succeeds for in-range depth");
               "classifyScreenMappingReject empty_grid for zero-dimension desc");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenMapping(0.5f, 0.5f, 10.f, zeroDesc, camera),
               "preflightDensityLookup succeeds for in-range index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(grid, desc, 3u, 1u, 2u) ==
               "preflightDensityLookupAtCoord succeeds for clampable OOB coords");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelPopulateReject(zeroDesc, camera, params) ==
               "classifyFroxelPopulateReject empty_desc for zero-dimension grid");
                   fuse::renderer::FroxelPopulateRejectReason::EmptyDesc),
    expectTrue(fuse::renderer::froxel_util::classifyFroxelPopulateReject(desc, badCamera, params) ==
               "classifyFroxelPopulateReject invalid_camera for bad near/far");
    testFroxelPreflightClassifyAndIsBlockingGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-4e8e ---
void testFroxelClassifyAndIsBlockingGuards() {
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenMapping(0.5f, 0.5f, 0.01f, desc, camera),
               "preflightScreenMapping rejects depth below near plane");
               "preflightDensityTrilinearSample succeeds on accessible grid");
               "preflightDensityTrilinearSample rejects hard OOB coords");
               "preflightSampleCoords writes reject reason on success");
               "preflightSampleCoords reports none on success");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenMapping(0.5f, 0.5f, 200.f, desc, camera, &mapReason),
               "preflightScreenMapping writes reject reason on failure");
               "preflightScreenMapping reports depth_out_of_range on failure");
    testFroxelClassifyAndIsBlockingGuards();

// --- deepen additive from froxel-volumetric-b511-guards-5ae7 ---
                   fuse::renderer::SampleCoordRejectReason::EmptyGrid),
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordReject(inBounds, zeroDesc) ==
               "classifySampleCoordReject empty_grid for empty desc");
               "preflightScreenMapping succeeds for valid mapping");
               "classifyScreenMappingReject depth_out_of_range for below-near depth");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupRejectAtCoord(grid, desc, 1u, 1u, 2u) ==
               "classifyDensityLookupRejectAtCoord none for in-range coords");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupRejectAtCoord(grid, desc, 99u, 99u, 99u) ==
               "classifyDensityLookupRejectAtCoord index_out_of_range for OOB coords");
               "classifyFroxelTrilinearSampleReject invalid_sample_coords for hard OOB coords");
               "classifyFroxelPopulateReject invalid_camera for inverted camera");

// --- deepen additive from froxel-classify-isblocking-guards-e388 ---
    expectTrue(fuse::renderer::FroxelGridLayout::classifyFroxelSampleCoordReject(inBounds, desc) ==
               "classifyFroxelSampleCoordReject none for valid coords");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyFroxelSampleCoordReject(warnWeights, desc) ==
               "classifyFroxelSampleCoordReject invalid_weights for clampable weights");
    expectTrue(!fuse::renderer::sampleCoordRejectReasonIsBlocking(sampleReason),
    expectTrue(fuse::renderer::FroxelGridLayout::classifyFroxelSampleCoordReject(hardOob, desc) ==
               "classifyFroxelSampleCoordReject out_of_bounds for hard OOB tile");
    expectTrue(fuse::renderer::sampleCoordRejectReasonIsBlocking(sampleReason),
               "preflightScreenDepthMapping succeeds for valid mapping");
               "preflightScreenDepthMapping rejects below-near depth");
    expectTrue(fuse::renderer::densityLookupRejectReasonIsBlocking(lookupReason),
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleCoordReject(grid, desc, inBounds) ==
               "classifyFroxelSampleCoordReject with grid none for valid coords");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleCoordReject(grid, desc, hardOob) ==
               "classifyFroxelSampleCoordReject with grid out_of_bounds for hard OOB");
    expectTrue(!fuse::renderer::froxel_util::preflightDensitySampleAtCoords(grid, desc, hardOob, &sampleReason),
               "preflightDensitySampleAtCoords rejects hard OOB coords");
               "preflightDensityTrilinearSample succeeds for valid coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityTrilinearSample(grid, desc, warnWeights, &trilinearReason),
    expectTrue(!fuse::renderer::froxelTrilinearSampleRejectReasonIsBlocking(trilinearReason),
    expectTrue(!fuse::renderer::froxel_util::preflightDensityTrilinearSample(grid, desc, hardOob, &trilinearReason),
    expectTrue(fuse::renderer::froxelTrilinearSampleRejectReasonIsBlocking(trilinearReason),
    expectTrue(fuse::renderer::froxelPopulateRejectReasonIsBlocking(populateReason),
    expectTrue(fuse::renderer::FroxelGridLayout::classifyFroxelSampleCoordReject(inBounds, zeroDesc) ==
               "classifyFroxelSampleCoordReject empty_grid for zero desc");
               "classifyFroxelPopulateReject empty_desc for zero desc");
               "classifyGridDensityReject none vacuously for empty desc");

// --- deepen additive from b511-froxel-classify-preflight-336a ---
               "classifyDensityLookupReject none for valid coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 99u, 99u, 99u),
               "preflightDensityLookup succeeds for clampable OOB coords");
               "classifyTrilinearSampleReject none for valid coords");
               "preflightTrilinearSample succeeds for valid coords");
    expectTrue(fuse::renderer::froxel_util::classifyTrilinearSampleReject(grid, desc, warnTrilinear) ==
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, warnTrilinear),
               "classifyTrilinearSampleReject invalid_sample_coords for hard OOB tile coord");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, hardOob),
               "classifyFroxelPopulateReject empty_desc for zero-dimension desc");

// --- deepen additive from deepen-froxel-volumetrics-b511-3f3e ---
void testFroxelClassifyAndBlockingGuards() {
               "classifySampleCoordReject reports invalid_weights for clampable weights");
               "classifySampleCoordReject reports out_of_bounds for hard OOB tile");
               "preflightSampleCoords reports out_of_bounds for hard OOB tile");
               "classifyScreenMappingReject reports depth_out_of_range below near plane");
               "preflightScreenMapping reports depth_out_of_range below near plane");
               "classifyDensityLookupReject reports none for in-range index");
               "preflightDensityLookup reports none for in-range index");
               "classifyFroxelTrilinearSampleReject reports none for in-bounds coords");
               "classifyFroxelTrilinearSampleReject reports clampable_weights for OOB weights");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSample(grid, desc, warnWeights, &trilinearReason),
               "preflightFroxelTrilinearSample reports clampable_weights for OOB weights");
               "classifyFroxelTrilinearSampleReject reports invalid_sample_coords for hard OOB tile");
               "classifyGridDensityReject reports none for accessible grid");
               "preflightGridDensity reports none for accessible grid");
               "classifyGridDensityReject reports undersized_storage for short buffer");
               "preflightGridDensity reports undersized_storage for short buffer");
               "classifyGridDensityReject vacuously reports none for empty desc");
    expectTrue(fuse::renderer::froxel_util::preflightGridDensity(grid, zeroDesc, &densityReason),
               "classifyFroxelPopulateReject reports none for valid inputs");
               "preflightFroxelPopulate reports none for valid inputs");
               "classifyFroxelPopulateReject reports zero_density for zero density");
               "preflightFroxelPopulate reports zero_density for zero density");

// --- deepen additive from deepen-froxel-volumetrics-b511-1f1f ---
               "preflightScreenMapping rejects below-near depth");
               "classifyGridDensityReject none for empty desc (vacuous)");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(zeroDesc, camera, params),
               "classifyFroxelPopulateReject invalid_camera for inverted near/far");

// --- deepen additive from deepen-froxel-classify-isblocking-34dc ---
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, warnWeights),
               "classifyTrilinearSampleReject invalid_sample_coords for hard OOB");
               "classifyGridDensityReject vacuously none for empty desc");
               "classifyPopulateReject none for valid populate");
    expectTrue(fuse::renderer::froxel_util::preflightPopulate(desc, camera, params),
               "preflightPopulate succeeds for valid populate");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulate(desc, camera, zeroDensity),
               "preflightPopulate rejects zero density");
               "classifyPopulateReject invalid_camera for bad camera");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulate(desc, badCamera, params),
               "preflightPopulate rejects invalid camera");

// --- deepen additive from deepen-froxel-volumetrics-b511-7c28 ---
               "classifySampleCoordsReject none for in-bounds coords");
               "classifySampleCoordsReject out_of_bounds for hard OOB tile coord");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordsReject(inBounds, zeroDesc) ==
               "classifySampleCoordsReject empty_grid for empty desc");
               "preflightScreenMapping reports depth_out_of_range for below-near depth");
               "classifyTrilinearSampleReject clampable_weights for OOB interpolation weights");
               "preflightTrilinearSample reports clampable_weights for OOB interpolation weights");
               "classifyTrilinearSampleReject inaccessible_grid for empty storage");
               "preflightPopulate succeeds for valid inputs");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulate(desc, camera, zeroDensity, &populateReason),
               "preflightPopulate reports zero_density for zero density");
    expectTrue(fuse::renderer::froxel_util::classifyPopulateReject(zeroDesc, camera, params) ==
               "classifyPopulateReject empty_desc for empty grid");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulate(zeroDesc, camera, params, &populateReason),
               "preflightPopulate rejects empty desc");
               "preflightPopulate reports empty_desc for empty grid");
    expectTrue(fuse::renderer::froxel_util::preflightPopulate(desc, camera, params) ==
               "preflightPopulate agrees with canPopulateFromAnalyticFog for valid inputs");
    expectTrue(fuse::renderer::froxel_util::preflightGridDensity(grid, desc) ==
               "preflightGridDensity agrees with validateGridDensity for accessible grid");

// --- deepen additive from deepen-froxel-volumetrics-b511-1e4c ---
               "preflightScreenMapping returns mapped coords on success");
               "preflightScreenMapping returns in-range sample coords");
               "classifyFroxelPopulateReject empty_desc");

// --- deepen additive from froxel-volumetric-b511-deepen-b4e6 ---
               "classifyScreenDepthMappingReject none for valid mapping");
               "preflightScreenDepthToSampleCoords succeeds for valid mapping");
               "preflightDensityLookupAtIndex succeeds for clampable OOB index");
               "classifyFroxelPopulateReject invalid_camera");

// --- deepen additive from deepen-froxel-volumetrics-b511-a55a ---
void testFroxelClassifyIsBlockingAndPreflightGuards() {
    expectTrue(fuse::renderer::preflightScreenMapping(0.5f, 0.5f, 10.f, desc, camera),
    expectTrue(!fuse::renderer::preflightScreenMapping(0.5f, 0.5f, 0.01f, desc, camera, &mapReason),
               "preflightScreenMapping reports depth_out_of_range");
    expectTrue(fuse::renderer::sampleCoordRejectReasonIsBlocking(fuse::renderer::SampleCoordRejectReason::OutOfBounds),
               "classifySampleCoordsReject invalid_weights for OOB interpolation weight");
               "preflightSampleCoords still succeeds for clampable weights");
               "classifyFroxelTrilinearSampleReject none for accessible grid");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelTrilinearSampleReject(emptyGrid, desc, inBounds) ==
               "classifyFroxelTrilinearSampleReject inaccessible_grid for empty storage");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleReject(grid, desc, inBounds) ==
               "classifyFroxelSampleReject none for accessible grid");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelSample(grid, desc, inBounds),
               "preflightFroxelSample succeeds for accessible grid");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleReject(emptyGrid, desc, inBounds) ==
               "classifyFroxelSampleReject out_of_bounds for empty storage");
    expectTrue(!fuse::renderer::froxelPopulateRejectReasonIsBlocking(fuse::renderer::FroxelPopulateRejectReason::None),
               "classifyFroxelPopulateReject invalid_camera for inverted planes");
    testFroxelClassifyIsBlockingAndPreflightGuards();

// --- deepen additive from deepen-b511-froxel-guards-7b26 ---
void testFroxelRejectClassificationAndPreflightGuards() {
               "classifyDensityLookupReject reports none for accessible grid");
               "preflightDensityLookup succeeds when only clamp warning");
               "preflightDensityLookup with reason succeeds on accessible grid");
               "preflightDensityLookup reports no reject reason on success");
               "classifyDensityLookupReject reports empty_storage for empty grid");
               "preflightSampleCoords with reason succeeds for in-bounds coords");
               "preflightSampleCoords reports no reject reason on success");
    expectTrue(fuse::renderer::froxel_util::classifyDensitySampleReject(grid, desc, inBounds) ==
               "classifyDensitySampleReject reports none for accessible grid");
               "preflightDensitySampleAtCoords succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::preflightDensityTrilinearSample(grid, desc, inBounds, &trilinearReason),
               "preflightDensityTrilinearSample with reason succeeds on accessible grid");
               "preflightDensityTrilinearSample reports no reject reason on success");
               "classifyScreenMappingReject reports none for in-range depth");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenDepthToSampleCoords(0.5f, 0.5f, 10.f, desc, camera),
               "preflightScreenDepthToSampleCoords with outputs succeeds in range");
               "preflightScreenDepthToSampleCoords reports no reject reason on success");
    expectTrue(fuse::renderer::froxel_util::preflightDensityAtScreen(grid, desc, camera, 0.5f, 0.5f, 10.f),
               "preflightDensityAtScreen succeeds on accessible grid");
               "preflightDensityAtScreen maps empty storage to empty_grid screen reject");
               "preflightGridDensity with reason succeeds on accessible grid");
               "preflightGridDensity reports no reject reason on success");
               "preflightFroxelPopulate with reason succeeds for valid inputs");
               "preflightFroxelPopulate reports no reject reason on success");
    testFroxelRejectClassificationAndPreflightGuards();

// --- deepen additive from deepen-b511-froxel-classify-guards-10ab ---
void testFroxelClassifyRejectAndPreflightReadyGuards() {
    expectTrue(fuse::renderer::preflightScreenMappingReady(0.5f, 0.5f, 10.f, desc, camera),
               "preflightScreenMappingReady succeeds for valid mapping");
    expectTrue(!fuse::renderer::preflightScreenMappingReady(0.5f, 0.5f, 10.f, zeroDesc, camera, &mapReason),
               "preflightScreenMappingReady rejects empty grid");
               "preflightScreenMappingReady reports empty_grid reject reason");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoordsReady(inBounds, desc),
               "preflightSampleCoordsReady succeeds for in-bounds coords");
        fuse::renderer::FroxelGridLayout::classifySampleCoordReject(warnWeights, desc);
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoordsReady(warnWeights, desc, &sampleReason),
               "preflightSampleCoordsReady succeeds for clampable weights");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightSampleCoordsReady(hardOob, desc, &sampleReason),
               "preflightSampleCoordsReady rejects hard OOB coords");
               "preflightSampleCoordsReady reports out_of_bounds reject reason");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupReady(grid, desc, 0u),
               "preflightDensityLookupReady succeeds for accessible grid");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupReady(grid, desc, 999u, &lookupReason),
               "preflightDensityLookupReady succeeds for clampable OOB index");
               "preflightDensityLookupReady reports index_out_of_range for OOB index");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoordReady(grid, desc, 99u, 99u, 99u),
               "preflightDensityLookupAtCoordReady succeeds for clampable OOB coords");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookupReady(emptyGrid, desc, 0u, &lookupReason),
               "preflightDensityLookupReady rejects empty storage");
               "preflightDensityLookupReady reports empty_storage reject reason");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSampleReady(grid, desc, inBounds),
               "preflightFroxelTrilinearSampleReady succeeds for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSampleReady(grid, desc, warnWeights, &trilinearReason),
               "preflightFroxelTrilinearSampleReady succeeds for clampable weights");
               "preflightFroxelTrilinearSampleReady reports clampable_weights reject reason");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelTrilinearSampleReady(grid, desc, hardOob, &trilinearReason),
               "preflightFroxelTrilinearSampleReady rejects hard OOB coords");
               "preflightFroxelTrilinearSampleReady reports invalid_sample_coords reject reason");
                   fuse::renderer::froxel_util::classifyGridDensityReject(emptyGrid, zeroDesc)),
    expectTrue(fuse::renderer::froxel_util::preflightFroxelPopulateReady(desc, camera, params),
               "preflightFroxelPopulateReady succeeds for valid populate inputs");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulateReady(desc, camera, zeroDensity, &populateReason),
               "preflightFroxelPopulateReady rejects zero density");
               "preflightFroxelPopulateReady reports zero_density reject reason");
               "classifyFroxelPopulateReject empty_desc for empty grid");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulateReady(zeroDesc, camera, params, &populateReason),
               "preflightFroxelPopulateReady rejects empty desc");
               "preflightFroxelPopulateReady reports empty_desc reject reason");
               "classifyFroxelPopulateReject invalid_camera for bad camera");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulateReady(desc, badCamera, params, &populateReason),
               "preflightFroxelPopulateReady rejects invalid camera");
               "preflightFroxelPopulateReady reports invalid_camera reject reason");
    testFroxelClassifyRejectAndPreflightReadyGuards();

// --- deepen additive from deepen-b511-froxel-volumetrics-5ada ---
               "preflightDensityLookup still succeeds for clampable OOB index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupCoordReject(grid, desc, 3u, 1u, 2u) ==
               "classifyDensityLookupCoordReject none for in-range coords");
               "preflightFroxelTrilinearSample still succeeds for clampable weights");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelTrilinearSample(emptyGrid, desc, inBounds),
               "preflightScreenDepthMapping returns mapped sample coords");

// --- deepen additive from deepen-b511-froxel-guards-d9de ---
void testFroxelClassifyPreflightAndBilinearGuards() {
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelMarch(grid, desc),
               "uniformly zero grid skips froxel march via wouldSkip");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelMarch(grid, desc) ==
               "wouldSkipFroxelMarch agrees with shouldSkipFroxelMarch");
                               fuse::renderer::ScreenMappingRejectReason::InvalidCamera),
    expectTrue(!fuse::renderer::wouldSkipScreenMapping(0.5f, 0.5f, 10.f, desc, camera),
    expectTrue(fuse::renderer::wouldSkipScreenMapping(0.5f, 0.5f, 0.01f, desc, camera),
               "preflightScreenMapping succeeds for valid inputs");
    expectTrue(fuse::renderer::classifySampleCoordReject(inBounds, desc) ==
    expectTrue(fuse::renderer::preflightSampleCoords(inBounds, desc),
    expectTrue(fuse::renderer::classifySampleCoordReject(inBounds, zeroDesc) ==
    expectTrue(fuse::renderer::preflightDensityLookup(grid, desc, 0u),
    expectTrue(fuse::renderer::classifyDensityLookupRejectAtCoord(grid, desc, 99u, 99u, 99u) ==
                   fuse::renderer::GridDensityRejectReason::DensityCountMismatch),
    expectTrue(fuse::renderer::classifyGridDensityReject(grid, desc) ==
               "tryValidateGridDensityForDesc succeeds for accessible grid");
               "tryValidateGridDensityForDesc reports no reject reason");
                               fuse::renderer::FroxelPopulateRejectReason::ZeroMarchSteps),
    expectTrue(fuse::renderer::preflightFroxelPopulate(desc, camera, params),
    expectTrue(fuse::renderer::froxel_util::classifyFroxelBilinearSampleReject(grid, desc, warnWeights) ==
               "classifyFroxelBilinearSampleReject clampable_weights for OOB weights");
               "trySampleDensityAtScreen with lookup reason succeeds on accessible grid");
               "trySampleDensityAtScreen with lookup reason rejects desc mismatch");
    expectNear(rejectedScreen, 0.f, 1e-6f, "trySampleDensityAtScreen with lookup reason zeroes output on rejection");
    testFroxelClassifyPreflightAndBilinearGuards();

// --- deepen additive from deepen-b511-froxel-classify-guards-1c1d ---
               "preflightSampleCoords reports out_of_bounds reject reason");
               "preflightFroxelTrilinearSample reports no reject reason");
               "preflightGridDensity reports no reject reason");
               "preflightFroxelPopulate reports no reject reason");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(grid, zeroDesc, 0u) ==
               "classifyDensityLookupReject empty_grid for empty desc");

// --- deepen additive from deepen-froxel-volumetrics-b511-114a ---
    expectTrue(fuse::renderer::densityLookupRejectReasonIsBlocking(fuse::renderer::DensityLookupRejectReason::EmptyGrid),
               "preflightDensityLookupAtIndex reports empty_storage reject reason");
    expectTrue(fuse::renderer::classifyScreenDepthMappingReject(0.5f, 0.5f, 10.f, desc, camera) ==
    expectTrue(fuse::renderer::classifyScreenDepthMappingReject(0.5f, 0.5f, 0.01f, desc, camera) ==
               "classifyScreenDepthMappingReject depth_out_of_range for below-near depth");

// --- deepen additive from deepen-froxel-volumetrics-b511-be15 ---
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupCoord(grid, desc, 1u, 1u, 2u),
               "preflightDensityLookupCoord succeeds for in-range coords");

// --- deepen additive from deepen-froxel-volumetrics-b511-2137 ---
void testFroxelRejectClassifyAndPreflightGuards() {
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(grid, desc, 5u) ==
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 5u),
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupAtCoordReject(grid, desc, 1u, 1u, 2u) ==
               "classifyDensityLookupAtCoordReject none for valid coord");
               "preflightDensityLookupAtCoord succeeds for valid coord");
               "classifyDensityLookupAtCoordReject index_out_of_range for OOB coord");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelPopulateReject(desc, camera, zeroMarch) ==
               "classifyFroxelPopulateReject zero_march_steps for zero march");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(desc, camera, zeroMarch),
               "preflightFroxelPopulate rejects zero march steps");
    testFroxelRejectClassifyAndPreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-c27f ---
               "preflightScreenDepthToSampleCoords rejects empty grid");

// --- deepen additive from deepen-b511-classify-guards-05f2 ---
               "preflightDensityLookup reports empty_storage on rejection");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyFroxelSampleCoordsReject(inBounds, desc) ==
               "classifyFroxelSampleCoordsReject none for in-bounds coords");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyFroxelSampleCoordsReject(warnWeights, desc) ==
               "classifyFroxelSampleCoordsReject invalid_weights for clampable weights");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyFroxelSampleCoordsReject(hardOob, desc) ==
               "classifyFroxelSampleCoordsReject out_of_bounds for hard OOB tile coord");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightFroxelSampleCoords(inBounds, desc, &sampleReason),
               "preflightFroxelSampleCoords reports no reject reason on success");
               "preflightFroxelSampleCoords succeeds for clampable weights");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightFroxelSampleCoords(hardOob, desc, &sampleReason),
               "preflightFroxelSampleCoords reports out_of_bounds for hard OOB tile coord");
               "classifyFroxelTrilinearSampleReject invalid_sample_coords for hard OOB tile coord");
               "preflightFroxelTrilinearSample reports no reject reason on success");
               "preflightFroxelPopulate reports zero_density on rejection");

// --- deepen additive from deepen-froxel-volumetrics-b511-821a ---
               "preflightDensityLookup succeeds when OOB index clamps");
               "classifyDensityLookupRejectAtCoord none for valid coords");
               "preflightDensityLookupAtCoord succeeds when OOB coords clamp");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenDepthMapping(0.5f, 0.5f, 10.f, zeroDesc, camera),
               "preflightScreenDepthMapping rejects empty froxel desc");

// --- deepen additive from deepen-froxel-volumetrics-b511-997a ---
    expectTrue(fuse::renderer::sampleCoordRejectReasonIsBlocking(fuse::renderer::SampleCoordRejectReason::EmptyGrid),
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 99u, 99u, 99u, &lookupReason),
               "preflightDensityTrilinearSample reports clampable_weights");
               "preflightFroxelPopulate reports zero_density");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenDepthToSampleCoordsReject(
               "classifyScreenDepthToSampleCoordsReject none for in-range depth");
               "classifyScreenDepthToSampleCoordsReject depth_out_of_range for below-near depth");
               "classifyScreenDepthToSampleCoordsReject empty_grid for empty desc");

// --- deepen additive from deepen-froxel-volumetrics-b511-e4d8 ---
               "preflightDensityLookup reports index_out_of_range for clampable OOB index");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMapping(0.5f, 0.5f, 10.f, desc, camera, &mapReason),
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenMapping(0.5f, 0.5f, 0.01f, desc, camera, &mapReason),
               "preflightScreenMapping reports depth_out_of_range reject reason");

// --- deepen additive from deepen-froxel-volumetrics-b511-9af2 ---
    using fuse::renderer::DensityLookupRejectReason;
    using fuse::renderer::FroxelPopulateRejectReason;
    using fuse::renderer::FroxelTrilinearSampleRejectReason;
    using fuse::renderer::GridDensityRejectReason;
    using fuse::renderer::SampleCoordRejectReason;
    using fuse::renderer::ScreenMappingRejectReason;
    expectTrue(!fuse::renderer::screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason::None),
    expectTrue(fuse::renderer::screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason::EmptyGrid),
    expectTrue(!fuse::renderer::sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason::None),
    expectTrue(!fuse::renderer::sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason::InvalidWeights),
    expectTrue(fuse::renderer::sampleCoordRejectReasonIsBlocking(SampleCoordRejectReason::OutOfBounds),
    expectTrue(!fuse::renderer::gridDensityRejectReasonIsBlocking(GridDensityRejectReason::None),
    expectTrue(fuse::renderer::gridDensityRejectReasonIsBlocking(GridDensityRejectReason::UndersizedStorage),
    expectTrue(!fuse::renderer::densityLookupRejectReasonIsBlocking(DensityLookupRejectReason::IndexOutOfRange),
    expectTrue(fuse::renderer::densityLookupRejectReasonIsBlocking(DensityLookupRejectReason::EmptyStorage),
    expectTrue(!fuse::renderer::froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason::None),
    expectTrue(fuse::renderer::froxelPopulateRejectReasonIsBlocking(FroxelPopulateRejectReason::ZeroDensity),
                   0.5f, 0.5f, 10.f, desc, camera) == ScreenMappingRejectReason::None,
               "classifyScreenDepthMappingReject succeeds in range");
                   0.5f, 0.5f, 10.f, zeroDesc, camera) == ScreenMappingRejectReason::EmptyGrid,
               "classifyScreenDepthMappingReject reports empty grid");
    fuse::renderer::ScreenMappingRejectReason mapReason = ScreenMappingRejectReason::None;
    expectTrue(mapReason == ScreenMappingRejectReason::None, "preflightScreenDepthMapping reports none");
    expectTrue(mapReason == ScreenMappingRejectReason::DepthOutOfRange,
               "preflightScreenDepthMapping reports depth_out_of_range");
    fuse::renderer::SampleCoordRejectReason sampleReason = SampleCoordRejectReason::None;
    expectTrue(sampleReason == SampleCoordRejectReason::None, "preflightSampleCoords reports none");
    expectTrue(sampleReason == SampleCoordRejectReason::OutOfBounds,
               "preflightSampleCoords reports out_of_bounds");
               "classifyDensityLookupReject reports none for origin index");
               "classifyDensityLookupCoordReject reports index_out_of_range for OOB coords");
    fuse::renderer::DensityLookupRejectReason lookupReason = DensityLookupRejectReason::None;
    expectTrue(lookupReason == DensityLookupRejectReason::None, "preflightDensityLookup reports none");
               "preflightDensityLookupAtCoord succeeds on in-range coords");
               "classifyDensityLookupReject reports empty_storage");
    expectTrue(lookupReason == DensityLookupRejectReason::EmptyStorage,
               "preflightDensityLookup reports empty_storage");
               "classifyFroxelTrilinearSampleReject reports clampable_weights");
               "classifyFroxelTrilinearSampleReject reports invalid_sample_coords");
    fuse::renderer::FroxelTrilinearSampleRejectReason trilinearReason = FroxelTrilinearSampleRejectReason::None;
    expectTrue(trilinearReason == FroxelTrilinearSampleRejectReason::None,
               "preflightFroxelTrilinearSample reports none");
    expectTrue(fuse::renderer::froxel_util::classifyGridDensityReject(grid, desc) == GridDensityRejectReason::None,
    fuse::renderer::FroxelPopulateRejectReason populateReason = FroxelPopulateRejectReason::None;
    expectTrue(populateReason == FroxelPopulateRejectReason::None, "preflightFroxelPopulate reports none");
    expectTrue(populateReason == FroxelPopulateRejectReason::ZeroDensity,
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(inBounds, desc, sampleReason) ==
               "tryPreflightSampleCoords agrees with preflightSampleCoords");
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 5u, lookupReason) ==
               "tryCanLookupAtIndex agrees with preflightDensityLookup");
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, params, populateReason) ==
               "tryCanPopulateFromAnalyticFog agrees with preflightFroxelPopulate");

// --- deepen additive from froxel-volumetric-b511-deepen-ea42 ---
    expectTrue(fuse::renderer::froxel_util::classifyFroxelDensityLookupReject(grid, desc, 0u) ==
               "classifyFroxelDensityLookupReject none for in-range index");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelDensityLookupReject(grid, desc, 999u) ==
               "classifyFroxelDensityLookupReject index_out_of_range for OOB index");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelDensityLookup(grid, desc, 999u),
               "preflightFroxelDensityLookup succeeds when OOB index would clamp");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelDensityLookupCoordReject(grid, desc, 99u, 99u, 99u) ==
               "classifyFroxelDensityLookupCoordReject index_out_of_range for OOB coords");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelDensityLookupAtCoord(grid, desc, 99u, 99u, 99u),
               "preflightFroxelDensityLookupAtCoord succeeds when OOB coords would clamp");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelDensityLookupReject(emptyGrid, desc, 0u) ==
               "classifyFroxelDensityLookupReject empty_storage for empty grid");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelDensityLookup(emptyGrid, desc, 0u),
               "preflightFroxelDensityLookup rejects empty storage");
               "preflightFroxelSampleCoords succeeds when weights would clamp");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyFroxelScreenMappingReject(
               "classifyFroxelScreenMappingReject none for in-range depth");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightFroxelScreenMapping(0.5f, 0.5f, 10.f, desc, camera),
               "preflightFroxelScreenMapping succeeds for in-range depth");
               "classifyFroxelScreenMappingReject depth_out_of_range below near plane");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightFroxelScreenMapping(0.5f, 0.5f, 0.01f, desc, camera),
               "preflightFroxelScreenMapping rejects depth below near plane");
               "classifyFroxelTrilinearSampleReject clampable_weights for clampable weights");
               "preflightFroxelTrilinearSample succeeds when weights would clamp");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelGridDensityReject(grid, desc) ==
               "classifyFroxelGridDensityReject none for accessible grid");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelGridDensity(grid, desc),
               "preflightFroxelGridDensity succeeds for accessible grid");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelGridDensityReject(undersized, desc) ==
               "classifyFroxelGridDensityReject undersized_storage for short buffer");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelGridDensity(undersized, desc),
               "preflightFroxelGridDensity rejects undersized storage");

// --- deepen additive from deepen-froxel-volumetrics-b511-128a ---
               "preflightScreenMapping reports no reject reason on success");
               "preflightScreenMapping reports depth_out_of_range on rejection");
               "preflightSampleCoords reports out_of_bounds on hard OOB rejection");
               "classifyTrilinearSampleReject clampable_weights for clampable weights");
               "classifyTrilinearSampleReject invalid_sample_coords for hard OOB coords");
               "preflightTrilinearSample reports no reject reason on success");
               "preflightTrilinearSample reports clampable_weights for clampable weights");
               "preflightTrilinearSample reports invalid_sample_coords on hard OOB rejection");
               "classifyGridDensityReject undersized_storage for undersized grid");
               "preflightGridDensity reports undersized_storage on rejection");

// --- deepen additive from deepen-froxel-b511-guards-fb8f ---
               "preflightDensityLookup succeeds when only clamp warning applies");
               "preflightDensityLookupCoord succeeds on accessible grid");
               "preflightSampleCoords succeeds when only weights will clamp");
               "preflightFroxelTrilinearSample succeeds when only weights will clamp");
               "classifyGridDensityReject none for zero-dimension desc (vacuous pass)");
               "preflightGridDensity succeeds vacuously for empty desc");
               "classifyFroxelPopulateReject zero_density for zero density param");
               "preflightFroxelPopulate rejects zero density param");
               "preflightSampleCoords agrees with canPreflightSampleCoords");
               "preflightDensityLookup agrees with canLookupAtIndex on accessible grid");
               "preflightFroxelPopulate agrees with canPopulateFromAnalyticFog");

// --- deepen additive from deepen-froxel-volumetrics-b511-45a3 ---
    expectTrue(fuse::renderer::preflightScreenDepthToSampleCoords(0.5f, 0.5f, 10.f, desc, camera),
               "preflightScreenDepthToSampleCoords succeeds for in-range depth");
    expectTrue(fuse::renderer::preflightScreenDepthToFroxelIndex(
               "preflightScreenDepthToFroxelIndex succeeds for in-range depth");
    expectTrue(mappedIndex < desc.froxelCount(), "preflightScreenDepthToFroxelIndex returns valid index");
    expectTrue(!fuse::renderer::preflightScreenDepthToSampleCoords(0.5f, 0.5f, 10.f, zeroDesc, camera),
               "preflightFroxelSampleCoords still succeeds for clampable weights");
               "classifyFroxelSampleCoordsReject out_of_bounds for hard OOB tile");
               "preflightFroxelSampleCoords rejects hard OOB tile");

// --- deepen additive from deepen-b511-froxel-classify-preflight-9601 ---
               "preflightDensityLookup succeeds with clamp warning for OOB index");
               "classifyDensityLookupAtCoordReject none for in-range coords");
               "preflightDensityLookupAtCoord succeeds with clamp warning for OOB coords");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityTrilinearSample(emptyGrid, desc, inBounds),
               "preflightDensityTrilinearSample rejects empty storage");

// --- deepen additive from deepen-froxel-volumetric-guards-15f7 ---
               "preflightDensityLookupAtIndex succeeds with clamp warning for OOB index");
               "preflightDensityTrilinearSample rejects hard OOB tile");

// --- deepen additive from deepen-froxel-volumetrics-b511-c843 ---
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenDepthToFroxelIndex(
               "preflightScreenDepthToFroxelIndex rejects empty grid");
               "preflightScreenDepthToSampleCoords returns mapped coords on success");
               "preflightScreenDepthToSampleCoords mapped coords in range");

// --- deepen additive from deepen-b511-froxel-classify-preflight-9310 ---
               "preflightDensityLookupAtIndex succeeds when OOB index would clamp");
               "preflightDensityLookupAtCoord succeeds when OOB coords would clamp");
    expectTrue(mappedCoords.sliceZ0 < desc.slicesZ, "preflightScreenDepthToSampleCoords returns mapped coords");
               "preflightScreenDepthToSampleCoords rejects depth below near plane");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelTrilinearSampleReject(grid, desc, warnTrilinear) ==
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSample(grid, desc, warnTrilinear),

// --- deepen additive from b511-froxel-classify-preflight-a761 ---
    expectTrue(froxelIndex < desc.froxelCount(), "preflightScreenDepthToFroxelIndex returns valid index");
               "preflightTrilinearSample reports clampable_weights reject reason");
               "preflightTrilinearSample reports invalid_sample_coords for hard OOB");
               "wouldSkipFroxelPopulate agrees with preflightFroxelPopulate for zero density");

// --- deepen additive from deepen-froxel-volumetrics-b511-6328 ---
    expectTrue(fuse::renderer::preflightDensityLookup(grid, desc, 999u),
    expectTrue(!fuse::renderer::preflightDensityLookup(emptyGrid, desc, 0u),
    expectTrue(fuse::renderer::classifyDensityLookupReject(grid, desc, 1u, 1u, 2u) ==
    expectTrue(fuse::renderer::classifyDensityLookupReject(grid, desc, 99u, 99u, 99u) ==
    expectTrue(fuse::renderer::preflightDensityLookup(grid, desc, 1u, 1u, 2u),
               "preflightDensityLookup succeeds for valid coords");
               "classifySampleCoordsReject invalid_weights for OOB weights");
    expectTrue(fuse::renderer::preflightScreenDepthMapping(0.5f, 0.5f, 10.f, desc, camera),
    expectTrue(fuse::renderer::preflightGridDensity(grid, desc),
    expectTrue(fuse::renderer::classifyGridDensityReject(undersized, desc) ==
    expectTrue(!fuse::renderer::preflightGridDensity(undersized, desc),
    expectTrue(!fuse::renderer::preflightFroxelPopulate(desc, camera, zeroDensity),

// --- deepen additive from deepen-b511-froxel-classify-preflight-2ecc ---
               "preflightSampleCoords succeeds when weights will clamp");
    expectTrue(mappedIndex < desc.froxelCount(), "preflightScreenDepthToFroxelIndex returns in-bounds index");
               "preflightFroxelTrilinearSample succeeds when weights will clamp");
                   fuse::renderer::GridDensityRejectReason::DescMismatch),
               "preflightScreenDepthMapping rejects empty grid");

// --- deepen additive from deepen-b511-froxel-guards-713a ---
               "classifySampleCoordReject reports out_of_bounds for hard OOB tile coord");
    expectTrue(mapped.sliceZ0 < desc.slicesZ, "preflightScreenDepthMapping returns mapped coords");
               "classifyDensityLookupRejectAtCoord reports index_out_of_range for OOB coords");
               "classifyFroxelSampleReject reports none for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelSampleAtCoords(grid, desc, inBounds),
               "preflightFroxelSampleAtCoords succeeds for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleReject(grid, desc, hardOob) ==
               "classifyFroxelSampleReject reports out_of_bounds for hard OOB coords");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelSampleAtCoords(grid, desc, hardOob),
               "preflightFroxelSampleAtCoords rejects hard OOB coords");
               "classifyFroxelTrilinearSampleReject reports invalid_sample_coords for hard OOB coords");
               "classifyFroxelPopulateReject reports none for valid populate inputs");
               "classifySampleCoordReject reports empty_grid for empty desc");

// --- deepen additive from deepen-b511-froxel-guards-80e2 ---
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMappingReady(0.5f, 0.5f, 10.f, desc, camera),
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenMappingReady(0.5f, 0.5f, 0.01f, desc, camera),
               "preflightScreenMappingReady rejects depth below near plane");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoordsReady(warnWeights, desc),
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightSampleCoordsReady(hardOob, desc),
               "preflightSampleCoordsReady rejects hard OOB tile coord");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupReady(grid, desc, 999u),
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupReady(grid, desc, 1u, 1u, 2u),
               "preflightDensityLookupReady succeeds for in-range coords");
    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookupReady(emptyGrid, desc, 0u),
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSampleReady(grid, desc, warnWeights),
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelTrilinearSampleReady(grid, desc, hardOob),
    expectTrue(fuse::renderer::froxel_util::preflightGridDensityReady(grid, desc),
               "preflightGridDensityReady succeeds for accessible grid");
    expectTrue(!fuse::renderer::froxel_util::preflightGridDensityReady(undersized, desc),
               "preflightGridDensityReady rejects undersized storage");
               "preflightFroxelPopulateReady succeeds for valid inputs");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulateReady(desc, camera, zeroDensity),
               "preflightFroxelPopulateReady rejects zero density param");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelPopulateReady(desc, camera, params) ==
               "preflightFroxelPopulateReady mirrors canPopulateFromAnalyticFog on valid path");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoordsReady(inBounds, desc) ==
               "preflightSampleCoordsReady mirrors canPreflightSampleCoords on valid path");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupReady(grid, desc, 0u) ==
               "preflightDensityLookupReady mirrors canLookupAtIndex on valid path");

// --- deepen additive from deepen-froxel-volumetrics-b511-ff11 ---
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMapping(0.5f, 0.5f, 10.f, desc, camera, nullptr,
               "preflightScreenMapping reports no reject reason");
               "preflightScreenMapping rejects empty desc");
    expectTrue(!fuse::renderer::sampleCoordRejectReasonIsBlocking(fuse::renderer::SampleCoordRejectReason::InvalidWeights),
               "preflightSampleCoords soft-succeeds for clampable weights");
               "preflightDensityLookup soft-succeeds for OOB index");
               "preflightDensityLookupAtCoord soft-succeeds for OOB coords");
               "preflightTrilinearSample soft-succeeds for clampable weights");
               "preflightTrilinearSample rejects hard OOB tile");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMapping(0.25f, 0.25f, 3.16f, desc, camera, &mapped),
               "preflightScreenMapping returns built sample coords");
               "preflightScreenMapping mapped coords in range");

// --- deepen additive from deepen-froxel-volumetrics-b511-8e12 ---
    expectTrue(mapped.tileX0 <= desc.tilesX - 1u, "preflightScreenDepthMapping returns in-bounds tile X");
               "classifyGridDensityReject undersized_storage for short storage");

// --- deepen additive from b511-froxel-volumetric-guards-3da9 ---
void testFroxelIsBlockingClassifyAndPreflightGuards() {
               "preflightDensityLookupAtCoord succeeds for accessible grid");
    testFroxelIsBlockingClassifyAndPreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-d9ce ---
void testFroxelIsBlockingAndPreflightGuards() {
    expectTrue(fuse::renderer::FroxelGridLayout::preflightMapScreenDepthToSampleCoords(
               "preflightMapScreenDepthToSampleCoords succeeds in range");
               "preflightMapScreenDepthToSampleCoords reports no reject reason in range");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightMapScreenDepthToFroxelIndex(
               "preflightMapScreenDepthToFroxelIndex succeeds in range");
    expectTrue(froxelIndex < desc.froxelCount(), "preflightMapScreenDepthToFroxelIndex returns valid index");
    expectTrue(!fuse::renderer::FroxelGridLayout::preflightMapScreenDepthToSampleCoords(
               "preflightMapScreenDepthToSampleCoords rejects empty grid");
               "preflightMapScreenDepthToSampleCoords reports empty_grid for empty desc");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenMappingFroxelIndexReject(
               "classifyScreenMappingFroxelIndexReject reports empty_grid for empty desc");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSampleAtCoords(grid, desc, inBounds, &trilinearReason),
               "preflightTrilinearSampleAtCoords succeeds on accessible grid");
               "preflightTrilinearSampleAtCoords reports no reject reason for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSampleAtCoords(grid, desc, warnWeights, &trilinearReason),
               "preflightTrilinearSampleAtCoords succeeds for clampable weights");
               "preflightTrilinearSampleAtCoords reports clampable_weights for OOB weights");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearSampleAtCoords(grid, desc, hardOob, &trilinearReason),
               "preflightTrilinearSampleAtCoords rejects hard OOB coords");
               "preflightTrilinearSampleAtCoords reports invalid_sample_coords for hard OOB coords");
               "preflightGridDensity reports no reject reason on accessible grid");
               "classifyGridDensityReject reports none on accessible grid");
    testFroxelIsBlockingAndPreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-54c3 ---
               "preflightGridDensity succeeds for empty desc");
               "preflightSampleCoords mirrors canPreflightSampleCoords for in-bounds coords");
               "preflightDensityLookup mirrors canLookupAtIndex for accessible grid");
               "preflightFroxelPopulate mirrors canPopulateFromAnalyticFog for valid inputs");

// --- deepen additive from deepen-froxel-volumetrics-b511-c8fa ---
void testFroxelBlockingClassifyAndPreflightGuards() {
    expectTrue(fuse::renderer::screenMappingRejectReasonIsBlocking(ScreenMappingRejectReason::DepthOutOfRange),
    expectTrue(!fuse::renderer::densityLookupRejectReasonIsBlocking(DensityLookupRejectReason::None),
               "preflightGridDensity rejects undersized grid");
    testFroxelBlockingClassifyAndPreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-5991 ---
void testFroxelDeepenPreflightAndClassifyGuards() {
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 1u, 1u, 2u),
               "preflightDensityLookup succeeds for in-range coords");
               "classifyFroxelSampleReject none for in-bounds coords");
               "preflightFroxelSample succeeds for in-bounds coords");
               "classifyFroxelSampleReject out_of_bounds for hard OOB coords");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelSample(grid, desc, hardOob),
               "preflightFroxelSample rejects hard OOB coords");
    testFroxelDeepenPreflightAndClassifyGuards();

// --- deepen additive from deepen-b511-froxel-preflight-ce10 ---
               "preflightDensityLookupAtCoord still succeeds for clampable OOB coords");

// --- deepen additive from deepen-b511-froxel-preflight-4caa ---
               "preflightDensityLookup succeeds when OOB coords would clamp");

// --- deepen additive from deepen-froxel-volumetrics-b511-1d0f ---
               "preflightDensityLookupReady succeeds when only clamp warning applies");
               "preflightDensityLookupReady with reason succeeds for clampable index");
               "preflightDensityLookupReady reports empty_storage for empty grid");
               "preflightDensityLookupAtCoordReady succeeds when only clamp warning applies");
               "preflightSampleCoordsReady reports invalid_weights for clampable weights");
               "preflightSampleCoordsReady reports out_of_bounds for hard OOB coords");
               "preflightScreenMappingReady with reason succeeds for valid mapping");
               "preflightScreenMappingReady reports none for valid mapping");
               "preflightScreenMappingReady rejects below-near depth");
               "preflightScreenMappingReady reports depth_out_of_range for below-near depth");
               "preflightFroxelTrilinearSampleReady reports clampable_weights for OOB weights");
               "preflightFroxelTrilinearSampleReady reports invalid_sample_coords for hard OOB coords");
    expectTrue(!fuse::renderer::froxel_util::preflightGridDensityReady(undersized, desc, &densityReason),
               "preflightGridDensityReady reports undersized_storage for undersized grid");
               "preflightFroxelPopulateReady reports zero_density for zero density");
               "preflightSampleCoordsReady mirrors canPreflightSampleCoords on valid coords");
               "preflightDensityLookupReady mirrors canLookupAtIndex on accessible grid");
               "preflightFroxelPopulateReady mirrors canPopulateFromAnalyticFog on valid inputs");

// --- deepen additive from deepen-froxel-volumetrics-b511-065c ---
void testFroxelDeepenIsBlockingClassifyAndPreflightGuards() {
               "preflightSampleCoords reports out_of_bounds sample reject reason");
               "preflightTrilinearSample reports clampable_weights trilinear reject reason");
               "preflightTrilinearSample reports invalid_sample_coords for hard OOB coords");
               "preflightFroxelPopulate reports zero_density populate reject reason");
    testFroxelDeepenIsBlockingClassifyAndPreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-034f ---
void testFroxelDeepenIsBlockingAndPreflightGuards() {
               "preflightScreenDepthToFroxelIndex succeeds for valid mapping");
               "preflightDensityLookup still succeeds for OOB index that clamps");
    testFroxelDeepenIsBlockingAndPreflightGuards();

// --- deepen additive from froxel-volumetric-b511-deepen-5b86 ---
    expectTrue(fuse::renderer::froxel_util::classifyFroxelTrilinearSampleReject(grid, desc, trilinearWarn) ==
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, trilinearWarn, &trilinearReason),
               "preflightSampleCoords mirrors canPreflightSampleCoords on valid coords");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtIndex(grid, desc, 0u) ==
               "preflightDensityLookupAtIndex mirrors canLookupAtIndex on accessible grid");
               "preflightFroxelPopulate mirrors canPopulateFromAnalyticFog on valid inputs");

// --- deepen additive from deepen-froxel-volumetrics-b511-81fc ---
               "classifySampleCoordReject reports out_of_bounds for hard OOB");
               "preflightSampleCoords rejects hard OOB coords");
               "preflightScreenMapping reports no reject reason in range");
               "classifyScreenMappingReject reports empty_grid for empty desc");
               "classifyDensityLookupRejectAtCoord reports none for in-range coords");
               "preflightTrilinearSample still succeeds for clampable weights");
               "classifyFroxelTrilinearSampleReject reports invalid_sample_coords for hard OOB");

// --- deepen additive from deepen-froxel-volumetrics-b511-f48a ---
               "preflightScreenMapping coords lie in grid");
    expectTrue(froxelIndex < desc.froxelCount(), "preflightScreenDepthToFroxelIndex returns in-bounds index");
                   0.5f, 0.5f, 10.f, zeroDesc, camera) == fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "preflightFroxelTrilinearSample reports clampable_weights");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(desc, badCamera, params, &populateReason),

// --- deepen additive from b511-froxel-volumetrics-deepen-6169 ---
               "preflightScreenMappingReady reports no reject reason on success");
               "preflightScreenMappingReady reports depth_out_of_range on rejection");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoordsReady(inBounds, desc, &sampleReason),
               "preflightSampleCoordsReady reports no reject reason on success");
               "preflightSampleCoordsReady still succeeds for clampable weights");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupReady(grid, desc, 0u, &lookupReason),
               "preflightDensityLookupReady succeeds for in-range index");
               "preflightDensityLookupReady reports no reject reason on success");
               "preflightDensityLookupReady still succeeds for OOB index that clamps");
               "preflightDensityLookupReady reports empty_storage on rejection");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSampleReady(grid, desc, inBounds, &trilinearReason),
               "preflightTrilinearSampleReady succeeds for in-bounds coords");
               "preflightTrilinearSampleReady reports no reject reason on success");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSampleReady(grid, desc, warnWeights, &trilinearReason),
               "preflightTrilinearSampleReady still succeeds for clampable weights");
               "preflightTrilinearSampleReady reports clampable_weights for OOB weights");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearSampleReady(grid, desc, hardOob, &trilinearReason),
               "preflightTrilinearSampleReady rejects hard OOB coords");
               "preflightTrilinearSampleReady reports invalid_sample_coords for hard OOB coords");
    expectTrue(fuse::renderer::froxel_util::preflightGridDensityReady(grid, desc, &densityReason),
               "preflightGridDensityReady reports no reject reason on success");
               "preflightGridDensityReady reports undersized_storage on rejection");
    expectTrue(fuse::renderer::froxel_util::preflightPopulateReady(desc, camera, params, &populateReason),
               "preflightPopulateReady succeeds for valid populate inputs");
               "preflightPopulateReady reports no reject reason on success");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulateReady(desc, camera, zeroDensity, &populateReason),
               "preflightPopulateReady rejects zero density");
               "preflightPopulateReady reports zero_density on rejection");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSampleReady(grid, desc, inBounds) ==
               "preflightTrilinearSampleReady mirrors canTrilinearSampleAtCoords on valid coords");
    expectTrue(fuse::renderer::froxel_util::preflightPopulateReady(desc, camera, params) ==
               "preflightPopulateReady mirrors canPopulateFromAnalyticFog on valid inputs");

// --- deepen additive from deepen-b511-froxel-guards-1c79 ---
               "classifySampleCoordReject reports out_of_bounds for hard OOB coords");
               "preflightScreenDepthToSampleCoords reports none in range");
               "preflightDensityLookup succeeds with OOB clamp warning");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 1u, 1u, 2u, &lookupReason),

// --- deepen additive from froxel-volumetric-b511-deepen-8c6b ---
void testFroxelDeepenGuardPredicates() {
               "preflightScreenMappingReady succeeds in range");
    expectTrue(!fuse::renderer::preflightScreenMappingReady(0.5f, 0.5f, 0.01f, desc, camera, &mapReason),
    expectTrue(fuse::renderer::preflightSampleCoordsReady(inBounds, desc),
    expectTrue(fuse::renderer::classifySampleCoordReject(warnWeights, desc) ==
    expectTrue(fuse::renderer::preflightSampleCoordsReady(warnWeights, desc, &sampleReason),
    expectTrue(fuse::renderer::preflightDensityLookupReady(grid, desc, 0u),
    expectTrue(fuse::renderer::preflightDensityLookupReady(grid, desc, 999u, &lookupReason),
               "preflightDensityLookupReady succeeds for OOB index that clamps");
    expectTrue(fuse::renderer::preflightDensityLookupAtCoordReady(grid, desc, 1u, 1u, 2u),
               "preflightDensityLookupAtCoordReady succeeds for in-range coords");
    expectTrue(!fuse::renderer::preflightDensityLookupReady(emptyGrid, desc, 0u, &lookupReason),
    expectTrue(fuse::renderer::preflightFroxelTrilinearSampleReady(grid, desc, inBounds),
    expectTrue(fuse::renderer::preflightFroxelTrilinearSampleReady(grid, desc, warnWeights, &trilinearReason),
    expectTrue(fuse::renderer::classifyFroxelTrilinearSampleReject(grid, desc, warnWeights) ==
    expectTrue(!fuse::renderer::preflightFroxelTrilinearSampleReady(grid, desc, hardOob, &trilinearReason),
               "preflightFroxelTrilinearSampleReady reports invalid_sample_coords for hard OOB");
    expectTrue(fuse::renderer::preflightGridDensityReady(grid, desc),
    expectTrue(!fuse::renderer::preflightGridDensityReady(undersized, desc, &densityReason),
    expectTrue(fuse::renderer::preflightFroxelPopulateReady(desc, camera, params),
    expectTrue(!fuse::renderer::preflightFroxelPopulateReady(desc, camera, zeroDensity, &populateReason),
    expectTrue(fuse::renderer::preflightSampleCoordsReady(inBounds, desc) ==
               "preflightSampleCoordsReady agrees with canPreflightSampleCoords");
    expectTrue(fuse::renderer::preflightFroxelPopulateReady(desc, camera, params) ==
               "preflightFroxelPopulateReady agrees with canPopulateFromAnalyticFog");
    expectTrue(fuse::renderer::preflightScreenMappingReady(0.5f, 0.5f, 0.01f, desc, camera) ==
               "preflightScreenMappingReady agrees with mapScreenDepthToSampleCoords rejection");

// --- deepen additive from deepen-froxel-volumetric-guards-3b64 ---
               "classifyScreenMappingReject none for in-range mapping");
               "preflightScreenMapping succeeds for in-range mapping");
    expectTrue(mapped.sliceZ0 < desc.slicesZ, "preflightScreenMapping returns mapped coords");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearSample(emptyGrid, desc, inBounds),
               "preflightTrilinearSample rejects empty storage");

// --- deepen additive from deepen-froxel-volumetric-guards-b511-4019 ---
               "preflightScreenMapping returns mapped sample coords");
    expectTrue(froxelIndex < desc.froxelCount(), "preflightScreenDepthToFroxelIndex returns in-range index");
               "classifyFroxelSampleReject none for valid coords");
               "preflightFroxelSample succeeds for valid coords");
               "classifyFroxelSampleReject invalid_weights for clampable weights");
               "classifyFroxelSampleReject out_of_bounds for hard OOB tile coord");
               "preflightFroxelSample rejects hard OOB tile coord");

// --- deepen additive from deepen-froxel-volumetric-guards-e872 ---
void testFroxelGuardClassifyPreflightAndIsBlocking() {
    expectTrue(fuse::renderer::preflightScreenMappingReady(0.5f, 0.5f, 10.f, desc, camera, &mapReason),
               "preflightScreenMappingReady reports none reject reason on success");
    expectTrue(fuse::renderer::classifySampleCoordReject(hardOob, desc) ==
    expectTrue(fuse::renderer::preflightSampleCoordsReady(inBounds, desc, &sampleReason),
               "preflightSampleCoordsReady reports none reject reason on success");
    expectTrue(!fuse::renderer::preflightSampleCoordsReady(hardOob, desc, &sampleReason),
    expectTrue(fuse::renderer::classifyDensityLookupRejectAtCoord(grid, desc, 1u, 1u, 2u) ==
    expectTrue(fuse::renderer::preflightDensityLookupReady(grid, desc, 0u, &lookupReason),
               "preflightDensityLookupReady reports none reject reason on success");
               "preflightDensityLookupReady still succeeds for clampable OOB index");
               "preflightDensityLookupReady reports index_out_of_range warning for OOB index");
    expectTrue(fuse::renderer::preflightDensityLookupAtCoordReady(grid, desc, 1u, 1u, 2u, &lookupReason),
    expectTrue(fuse::renderer::preflightTrilinearSampleReady(grid, desc, inBounds, &trilinearReason),
               "preflightTrilinearSampleReady reports none reject reason on success");
    expectTrue(fuse::renderer::preflightTrilinearSampleReady(grid, desc, warnWeights, &trilinearReason),
               "preflightTrilinearSampleReady reports clampable_weights warning");
    expectTrue(!fuse::renderer::preflightTrilinearSampleReady(grid, desc, hardOob, &trilinearReason),
    expectTrue(fuse::renderer::preflightGridDensityReady(grid, desc, &densityReason),
               "preflightGridDensityReady reports none reject reason on success");
    expectTrue(fuse::renderer::preflightFroxelPopulateReady(desc, camera, params, &populateReason),
               "preflightFroxelPopulateReady reports none reject reason on success");
    testFroxelGuardClassifyPreflightAndIsBlocking();

// --- deepen additive from deepen-froxel-volumetrics-b511-d893 ---
               "preflightGridDensity mirrors validateGridDensity on accessible grid");

// --- deepen additive from froxel-volumetric-b511-deepen-cd85 ---
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupReady(grid, desc, 99u, 99u, 99u),
               "preflightDensityLookupReady succeeds when coord lookup only clamps");
               "preflightScreenMappingReady succeeds for in-range depth");
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSampleReady(grid, desc, inBounds),
    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSampleReady(grid, desc, warnWeights),
               "preflightTrilinearSampleReady succeeds for clampable weights");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearSampleReady(grid, desc, hardOob),
    expectTrue(fuse::renderer::froxel_util::preflightGridDensityReady(grid, zeroDesc),
               "preflightGridDensityReady succeeds for empty desc");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulateReady(desc, badCamera, params),

// --- deepen additive from deepen-froxel-isblocking-preflight-c683 ---
               "preflightSampleCoords succeeds when weights would clamp");
               "preflightSampleCoords reports invalid_weights reject reason");

// --- deepen additive from deepen-froxel-volumetric-guards-83db ---
void testFroxelDeepenGuardPredicatesAndPreflights() {
               "preflightDensityLookupReady accepts accessible grid");
               "preflightDensityLookupReady accepts clampable OOB index");
               "preflightDensityLookupReady reports index_out_of_range for clampable index");
               "preflightDensityLookupReady mirrors canLookupAtIndex on valid grid");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupCoordReady(grid, desc, 99u, 99u, 99u, &lookupReason),
               "preflightDensityLookupCoordReady accepts clampable OOB coords");
               "preflightSampleCoordsReady accepts in-bounds coords");
               "preflightSampleCoordsReady mirrors canPreflightSampleCoords");
               "preflightSampleCoordsReady accepts clampable weights");
               "preflightSampleCoordsReady reports out_of_bounds for hard OOB tile coord");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMappingReady(0.5f, 0.5f, 10.f, desc, camera, &mapReason),
               "preflightScreenMappingReady accepts in-range mapping");
               "preflightFroxelTrilinearSampleReady accepts in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelTrilinearSampleReady(grid, desc, inBounds) ==
               "preflightFroxelTrilinearSampleReady mirrors canTrilinearSampleAtCoords");
               "preflightFroxelTrilinearSampleReady accepts clampable weights");
               "preflightFroxelTrilinearSampleReady reports clampable_weights for clampable weights");
               "preflightGridDensityReady accepts accessible grid");
    expectTrue(fuse::renderer::froxel_util::preflightGridDensityReady(grid, desc) ==
               "preflightGridDensityReady mirrors validateGridDensity on valid grid");
               "preflightGridDensityReady reports undersized_storage for short buffer");
    expectTrue(fuse::renderer::froxel_util::preflightGridDensityReady(grid, zeroDesc, &densityReason),
               "preflightGridDensityReady vacuously accepts empty desc");
               "preflightGridDensityReady reports no reject reason for empty desc");
    expectTrue(fuse::renderer::froxel_util::preflightFroxelPopulateReady(desc, camera, params, &populateReason),
               "preflightFroxelPopulateReady accepts valid inputs");
               "preflightFroxelPopulateReady reports no reject reason on success");
               "preflightFroxelPopulateReady mirrors canPopulateFromAnalyticFog");
               "preflightFroxelPopulateReady reports invalid_camera for bad camera");
    testFroxelDeepenGuardPredicatesAndPreflights();

// --- deepen additive from deepen-froxel-volumetrics-b511-c943 ---
void testFroxelGuardClassifyAndPreflightHelpers() {
               "preflightDensityLookupAtIndex still succeeds when index clamps");
               "preflightDensityLookupAtCoord reports empty_storage reject reason");
               "classifyScreenMappingReject none in range");
               "preflightScreenDepthMapping reports depth_out_of_range reject reason");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleAtCoordsReject(grid, desc, inBounds) ==
               "classifyFroxelSampleAtCoordsReject none for accessible grid");
               "preflightFroxelSampleAtCoords succeeds for accessible grid");
    testFroxelGuardClassifyAndPreflightHelpers();

// --- deepen additive from deepen-froxel-volumetric-guards-b511-5161 ---
    expectTrue(fuse::renderer::gridDensityRejectReasonIsBlocking(GridDensityRejectReason::DescMismatch),
               "preflightScreenMappingReady accepts valid mapping");
    expectTrue(mapReason == ScreenMappingRejectReason::None, "preflightScreenMappingReady reports none");
               "preflightScreenMappingReady reports depth_out_of_range");
    expectTrue(sampleReason == SampleCoordRejectReason::None, "preflightSampleCoordsReady reports none");
               "preflightSampleCoordsReady warns but accepts clampable weights");
    expectTrue(sampleReason == SampleCoordRejectReason::InvalidWeights,
               "preflightSampleCoordsReady reports invalid_weights");
               "preflightSampleCoordsReady reports out_of_bounds");
    expectTrue(lookupReason == DensityLookupRejectReason::None, "preflightDensityLookupReady reports none");
               "preflightDensityLookupReady warns but accepts OOB index");
    expectTrue(lookupReason == DensityLookupRejectReason::IndexOutOfRange,
               "preflightDensityLookupReady reports index_out_of_range");
               "preflightDensityLookupReady reports empty_storage");
    fuse::renderer::GridDensityRejectReason densityReason = GridDensityRejectReason::None;
    expectTrue(densityReason == GridDensityRejectReason::UndersizedStorage,
               "preflightGridDensityReady reports undersized_storage");
               "preflightTrilinearSampleReady accepts in-bounds coords");
               "preflightTrilinearSampleReady reports none");
               "preflightTrilinearSampleReady warns but accepts clampable weights");
    expectTrue(trilinearReason == FroxelTrilinearSampleRejectReason::ClampableWeights,
               "preflightTrilinearSampleReady reports clampable_weights");
    expectTrue(trilinearReason == FroxelTrilinearSampleRejectReason::InvalidSampleCoords,
               "preflightTrilinearSampleReady reports invalid_sample_coords");
               "preflightFroxelPopulateReady accepts valid populate inputs");
    expectTrue(populateReason == FroxelPopulateRejectReason::None, "preflightFroxelPopulateReady reports none");
               "preflightFroxelPopulateReady reports zero_density");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMappingReady(0.5f, 0.5f, 10.f, desc, camera) ==
               "preflightScreenMappingReady mirrors mapScreenDepthToSampleCoords on valid path");
               "preflightTrilinearSampleReady mirrors canTrilinearSampleAtCoords on valid path");

// --- deepen additive from froxel-volumetrics-b511-deepen-c27d ---
void testFroxelClassifyRejectAndPreflightGuards() {
    expectTrue(!fuse::renderer::preflightScreenMappingReady(0.5f, 0.5f, 0.01f, desc, camera),
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupCoordReady(grid, desc, 1u, 1u, 2u),
               "preflightDensityLookupCoordReady succeeds for in-range coords");
    expectTrue(fuse::renderer::preflightScreenMappingReady(0.5f, 0.5f, 10.f, desc, camera) ==
    testFroxelClassifyRejectAndPreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-2686 ---
               "preflightScreenMapping mapped tile coords in range");
    expectTrue(!fuse::renderer::gridDensityRejectReasonIsBlocking(densityReason),
               "preflightDensityLookup mirrors canLookupAtIndex on accessible grid");

// --- deepen additive from deepen-froxel-volumetrics-b511-0ad3 ---
void testFroxelDeepenClassifyAndPreflightGuards() {
               "preflightScreenDepthToSampleCoords passes for valid mapping");
               "preflightScreenDepthToSampleCoords fills coords on success");
    expectTrue(!fuse::renderer::preflightScreenDepthToFroxelIndex(0.5f, 0.5f, 0.01f, desc, camera),
               "preflightScreenDepthToFroxelIndex rejects below-near depth");
               "preflightFroxelSampleCoords passes for in-bounds coords");
               "preflightFroxelSampleCoords passes for clampable weights");
               "preflightDensityLookup passes for accessible grid");
               "preflightDensityLookup passes for OOB index that clamps");
               "preflightDensityLookupAtCoord passes for OOB coords that clamp");
               "classifyFroxelSampleCoordReject none for accessible grid");
               "preflightFroxelSampleAtCoords passes for accessible grid");
               "classifyFroxelSampleCoordReject out_of_bounds for hard OOB coords");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelSampleAtCoords(grid, desc, hardOob, &sampleReason),
               "preflightFroxelTrilinearSample passes for accessible grid");
               "preflightFroxelTrilinearSample passes for clampable weights");
               "preflightFroxelTrilinearSample reports clampable_weights reason");
               "preflightGridDensity passes for accessible grid");
               "preflightGridDensity passes vacuously for empty desc");
               "preflightFroxelPopulate passes for valid inputs");
    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(zeroDesc, camera, params, &populateReason),
    testFroxelDeepenClassifyAndPreflightGuards();

// --- deepen additive from deepen-froxel-volumetrics-b511-4fdc ---
void testFroxelBlockingClassifyAndPreflightWrappers() {
    fuse::renderer::DensityLookupRejectReason lookupReject = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::preflightDensityLookup(grid, desc, 0u, &lookupReject),
    expectTrue(lookupReject == fuse::renderer::DensityLookupRejectReason::None,
               "preflightDensityLookup reports no reject on in-range index");
               "classifyDensityLookupReject warns on OOB index");
    expectTrue(fuse::renderer::preflightDensityLookup(grid, desc, 999u, &lookupReject),
               "preflightDensityLookup still succeeds when index would clamp");
    expectTrue(lookupReject == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "preflightDensityLookup reports index_out_of_range warning");
    expectTrue(!fuse::renderer::preflightDensityLookup(emptyGrid, desc, 0u, &lookupReject),
    expectTrue(lookupReject == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "preflightDensityLookup reports empty_storage on empty grid");
    expectTrue(fuse::renderer::preflightDensityLookupAtCoord(grid, desc, 1u, 1u, 2u, &lookupReject),
               "classifyDensityLookupCoordReject warns on OOB coords");
    fuse::renderer::SampleCoordRejectReason sampleReject = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(inBounds, desc, &sampleReject),
               "preflightSampleCoords succeeds on in-bounds coords");
    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(warnWeights, desc, &sampleReject),
    expectTrue(sampleReject == fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "preflightSampleCoords reports invalid_weights warning");
    fuse::renderer::ScreenMappingRejectReason mapReject = fuse::renderer::ScreenMappingRejectReason::None;
    expectTrue(fuse::renderer::preflightScreenMapping(0.5f, 0.5f, 10.f, desc, camera, &mapReject),
    expectTrue(!fuse::renderer::preflightScreenMapping(0.5f, 0.5f, 0.01f, desc, camera, &mapReject),
    expectTrue(mapReject == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
    fuse::renderer::FroxelTrilinearSampleRejectReason trilinearReject =
    expectTrue(fuse::renderer::preflightFroxelTrilinearSample(grid, desc, inBounds, &trilinearReject),
               "preflightFroxelTrilinearSample succeeds on accessible grid");
               "classifyFroxelTrilinearSampleReject warns on clampable weights");
    expectTrue(fuse::renderer::preflightFroxelTrilinearSample(grid, desc, warnWeights, &trilinearReject),
    fuse::renderer::GridDensityRejectReason densityReject = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::preflightGridDensity(grid, desc, &densityReject),
    fuse::renderer::FroxelPopulateRejectReason populateReject = fuse::renderer::FroxelPopulateRejectReason::None;
    expectTrue(fuse::renderer::preflightFroxelPopulate(desc, camera, params, &populateReject),
    expectTrue(!fuse::renderer::preflightFroxelPopulate(desc, camera, zeroDensity, &populateReject),
    expectTrue(populateReject == fuse::renderer::FroxelPopulateRejectReason::ZeroDensity,
    testFroxelBlockingClassifyAndPreflightWrappers();
