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

void testFroxelLookupRejectReasons() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);

    fuse::renderer::FroxelLookupRejectReason reason = fuse::renderer::FroxelLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 0u, reason),
               "tryCanLookup accepts accessible grid");
    expectTrue(reason == fuse::renderer::FroxelLookupRejectReason::None, "accessible grid reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelLookupRejectReasonLabel(reason), "none") == 0,
               "none lookup reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, zeroDesc, 0u, reason),
               "tryCanLookup rejects empty grid desc");
    expectTrue(reason == fuse::renderer::FroxelLookupRejectReason::EmptyGrid,
               "empty grid reports empty_grid reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelLookupRejectReasonLabel(reason), "empty_grid") == 0,
               "empty grid lookup reject reason label");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(emptyGrid, desc, 0u, reason),
               "tryCanLookup rejects empty storage");
    expectTrue(reason == fuse::renderer::FroxelLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelLookupRejectReasonLabel(reason), "empty_storage") == 0,
               "empty storage lookup reject reason label");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, mismatched, 0u, reason),
               "tryCanLookup rejects desc mismatch");
    expectTrue(reason == fuse::renderer::FroxelLookupRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelLookupRejectReasonLabel(reason), "desc_mismatch") == 0,
               "desc mismatch lookup reject reason label");

    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled, reason),
               "trySample with reason succeeds on accessible grid");
    expectTrue(reason == fuse::renderer::FroxelLookupRejectReason::None,
               "successful trySample reports no reject reason");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, sampled, reason),
               "trySample with reason rejects empty storage");
    expectTrue(reason == fuse::renderer::FroxelLookupRejectReason::EmptyStorage,
               "trySample with reason reports empty_storage on rejection");
    expectNear(sampled, 0.f, 1e-6f, "trySample zeroes output on rejection");
}

void testFroxelSampleCoordRejectReasons() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleCoordRejectReason reason = fuse::renderer::FroxelSampleCoordRejectReason::None;
    fuse::renderer::FroxelSampleCoords coords{};
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, desc, camera, coords, reason),
               "tryMap sample coords accepts valid request");
    expectTrue(reason == fuse::renderer::FroxelSampleCoordRejectReason::None,
               "valid sample coord mapping reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelSampleCoordRejectReasonLabel(reason), "none") == 0,
               "none sample coord reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, coords, reason),
               "tryMap sample coords rejects empty grid");
    expectTrue(reason == fuse::renderer::FroxelSampleCoordRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelSampleCoordRejectReasonLabel(reason), "empty_grid") == 0,
               "empty grid sample coord reject reason label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, coords, reason),
               "tryMap sample coords rejects depth below near plane");
    expectTrue(reason == fuse::renderer::FroxelSampleCoordRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelSampleCoordRejectReasonLabel(reason), "depth_out_of_range") == 0,
               "depth out of range sample coord reject reason label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 200.f, desc, camera, coords, reason),
               "tryMap sample coords rejects depth above far plane");
    expectTrue(reason == fuse::renderer::FroxelSampleCoordRejectReason::DepthOutOfRange,
               "above-far depth reports depth_out_of_range reject reason");

    fuse::renderer::FroxelCameraDesc invalidCamera{};
    invalidCamera.nearPlane = 0.f;
    invalidCamera.farPlane = 100.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, desc, invalidCamera, coords, reason),
               "tryMap sample coords rejects invalid camera");
    expectTrue(reason == fuse::renderer::FroxelSampleCoordRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera sample coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelSampleCoordRejectReasonLabel(reason), "invalid_camera") == 0,
               "invalid camera sample coord reject reason label");

    fuse::u32 froxelIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, desc, camera, froxelIndex, reason),
               "tryMap froxel index accepts valid request");
    expectTrue(froxelIndex < desc.froxelCount(), "tryMap froxel index returns in-bounds index");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, froxelIndex, reason),
               "tryMap froxel index rejects empty grid");
    expectTrue(reason == fuse::renderer::FroxelSampleCoordRejectReason::EmptyGrid,
               "tryMap froxel index reports empty_grid on rejection");
}

void testFroxelTryClampSampleCoords() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

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
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(coords, desc),
               "tryClamp sample coords succeeds on non-empty grid");
    expectTrue(coords.tileX0 == 3u && coords.tileY0 == 1u && coords.sliceZ0 == 2u,
               "tryClamp sample coords clamps tile/slice corners");
    expectNear(coords.tx, 1.f, 1e-5f, "tryClamp sample coords clamps tx high");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::FroxelSampleCoords unclamped{};
    unclamped.tileX0 = 5u;
    unclamped.tx = 2.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(unclamped, zeroDesc),
               "tryClamp sample coords rejects empty grid");
    expectTrue(unclamped.tileX0 == 5u, "tryClamp leaves coords unchanged on empty grid");
    expectNear(unclamped.tx, 2.f, 1e-5f, "tryClamp leaves interpolation weights unchanged on empty grid");
}

void testFroxelTrySampleDensityGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 2;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, 0u, desc)] = 0.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 0u, 0u, desc)] = 1.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 1u, 0u, desc)] = 0.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 1u, 0u, desc)] = 1.f;

    fuse::renderer::FroxelSampleCoords coords{};
    coords.tileX0 = 0u;
    coords.tileY0 = 0u;
    coords.tileX1 = 1u;
    coords.tileY1 = 1u;
    coords.sliceZ0 = 0u;
    coords.sliceZ1 = 0u;
    coords.tx = 0.5f;
    coords.ty = 0.5f;

    fuse::f32 bilinear = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, coords, bilinear),
               "trySample bilinear succeeds on accessible grid");
    expectNear(bilinear, 0.5f, 1e-5f, "trySample bilinear returns interpolated density");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    fuse::f32 rejected = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(emptyGrid, desc, coords, rejected),
               "trySample bilinear rejects empty storage");
    expectNear(rejected, 0.f, 1e-6f, "trySample bilinear zeroes output on rejection");

    coords.sliceZ1 = 1u;
    coords.tz = 0.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, 1u, desc)] = 0.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 0u, 1u, desc)] = 1.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 1u, 1u, desc)] = 0.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 1u, 1u, desc)] = 1.5f;

    fuse::f32 trilinear = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, coords, trilinear),
               "trySample trilinear succeeds on accessible grid");
    expectNear(trilinear, 0.75f, 1e-5f, "trySample trilinear returns interpolated density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(emptyGrid, desc, coords, rejected),
               "trySample trilinear rejects empty storage");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::f32 screenDensity = 0.f;
    fuse::renderer::FroxelLookupRejectReason lookupReason = fuse::renderer::FroxelLookupRejectReason::None;
    fuse::renderer::FroxelSampleCoordRejectReason coordReason = fuse::renderer::FroxelSampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenDensity, lookupReason, coordReason),
               "trySample at screen succeeds on accessible grid");
    expectNear(screenDensity,
               fuse::renderer::froxel_util::sampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               1e-5f,
               "trySample at screen matches unguarded sample");
    expectTrue(lookupReason == fuse::renderer::FroxelLookupRejectReason::None,
               "successful screen sample reports no lookup reject reason");
    expectTrue(coordReason == fuse::renderer::FroxelSampleCoordRejectReason::None,
               "successful screen sample reports no coord reject reason");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   emptyGrid, desc, camera, 0.5f, 0.5f, 10.f, rejected, lookupReason, coordReason),
               "trySample at screen rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::FroxelLookupRejectReason::EmptyStorage,
               "screen sample on empty storage reports empty_storage lookup reason");
    expectTrue(coordReason == fuse::renderer::FroxelSampleCoordRejectReason::None,
               "screen sample on empty storage does not set coord reject reason");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, rejected, lookupReason, coordReason),
               "trySample at screen rejects out-of-range depth");
    expectTrue(lookupReason == fuse::renderer::FroxelLookupRejectReason::None,
               "screen sample depth rejection does not set lookup reject reason");
    expectTrue(coordReason == fuse::renderer::FroxelSampleCoordRejectReason::DepthOutOfRange,
               "screen sample depth rejection reports depth_out_of_range coord reason");
}

void testFroxelDensityAccessAndClampGuards() {
void testFroxelValidateGridDensityAndAccessGuards() {
void testFroxelDensityAccessGuardsAndValidation() {
void testFroxelMaxIndexDeepenHelpers() {
void testFroxelTryClampAndDensityAccessGuards() {
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
    expectTrue(desc.maxFroxelIndex() == 23u, "FroxelGridDesc maxFroxelIndex matches last cell");
               "FroxelGridLayout maxFroxelIndex delegates to FroxelGridDesc");

               "last froxel index is at max");

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
               "penultimate index is not at max froxel index");

               "try clamp froxel index succeeds on non-empty grid");
    expectTrue(clampedIndex == 17u, "try clamp preserves in-bounds index");

               "try clamp froxel index succeeds for OOB input");
    expectTrue(clampedOob == desc.maxFroxelIndex(), "try clamp maps OOB to max index");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::u32 emptyClamp = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampFroxelIndex(5u, zeroDesc, emptyClamp),
               "tryClamp rejects empty grid");
    expectTrue(emptyClamp == 0u, "tryClamp zeroes output on empty grid");
               "try clamp froxel index fails on empty grid");
    expectTrue(emptyClamp == 0u, "try clamp zeroes output on empty grid");
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

void testFroxelSampleCoordGuards() {
void testFroxelLookupAndSampleCoordGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;


    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelGrid(desc),
               "non-empty froxel desc does not skip grid ops");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelLookup(grid, desc),
               "allocated grid does not skip lookup");
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

    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(emptyGrid, desc, 0u, lookupReason),
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage reject reason");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelLookup(emptyGrid, desc),
               "empty storage skips lookup");

    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, mismatched, 0u, lookupReason),
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

void testFroxelSampleCoordNormalizeAndScreenMappingGuards() {

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
    extremeWeights.ty = -1.f;
    extremeWeights.tz = 3.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidSampleCoords(extremeWeights, desc),
               "OOB interpolation weights fail validity check");
    fuse::renderer::FroxelGridLayout::normalizeSampleCoords(extremeWeights);
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(extremeWeights, desc),
               "normalizeSampleCoords clamps interpolation weights");


    fuse::renderer::ScreenMappingRejectReason mapReason = fuse::renderer::ScreenMappingRejectReason::None;
    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, desc, camera, mapped, mapReason),
               "tryMapScreenDepthToSampleCoords succeeds in range");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::None,
               "successful screen mapping reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::screenMappingRejectReasonLabel(mapReason), "none") == 0,
               "none screen-mapping reject reason label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, mapped, mapReason),
               "tryMapScreenDepthToSampleCoords rejects empty grid");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "empty grid reports empty_grid screen-mapping reject reason");
    expectTrue(std::strcmp(fuse::renderer::screenMappingRejectReasonLabel(mapReason), "empty_grid") == 0,
               "empty_grid screen-mapping reject reason label");

                   0.5f, 0.5f, 0.01f, desc, camera, mapped, mapReason),
               "tryMapScreenDepthToSampleCoords rejects depth below near plane");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range reject reason");

    fuse::renderer::FroxelCameraDesc badCamera{};
    badCamera.nearPlane = 100.f;
    badCamera.farPlane = 1.f;
                   0.5f, 0.5f, 50.f, desc, badCamera, mapped, mapReason),
               "tryMapScreenDepthToSampleCoords rejects invalid camera");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera reject reason");

    fuse::u32 froxelIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, desc, camera, froxelIndex, mapReason),
               "tryMapScreenDepthToFroxelIndex succeeds in range");
    expectTrue(froxelIndex < desc.froxelCount(), "mapped froxel index in bounds");

    grid.density[0] = 1.f;

    inBounds.tileX0 = 0u;
    inBounds.tileX1 = 1u;
    inBounds.sliceZ0 = 0u;
    inBounds.sliceZ1 = 1u;
    inBounds.tx = 0.5f;
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

    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(emptyGrid, desc, inBounds, sampleReason),
               "tryCanSampleAtCoords rejects empty storage");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "empty storage maps to out_of_bounds sample reject reason");

    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, bilinearSample, sampleReason),
               "trySampleDensityBilinear with reason succeeds on accessible grid");
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

void testFroxelPopulatePreflightAndLookupGuards() {
    desc.tilesX = 2;
    desc.slicesZ = 2;

    expectTrue(fuse::renderer::froxel_util::validateGridDensity(emptyGrid, zeroDesc),
               "empty storage validates against empty froxel desc");
    expectTrue(!fuse::renderer::froxel_util::canAccessDensityGrid(emptyGrid, desc),
               "empty storage cannot access density grid");
    expectTrue(!fuse::renderer::froxel_util::canAccessDensityGrid(emptyGrid, zeroDesc),
               "empty storage cannot access zero-dimension desc");

    expectTrue(fuse::renderer::froxel_util::canAccessDensityGrid(grid, desc),
               "allocated grid grants density access");
    expectTrue(fuse::renderer::froxel_util::validateGridDensity(grid, desc),
               "fresh allocate validates grid density");
    expectTrue(!fuse::renderer::froxel_util::validateGridDensity(grid, mismatched),
               "grid density rejects desc mismatch");
    expectTrue(!fuse::renderer::froxel_util::validateGridDensity(grid, zeroDesc),
               "non-empty storage rejects empty froxel desc");
    expectTrue(!fuse::renderer::froxel_util::canAccessDensityGrid(grid, mismatched),
               "desc mismatch denies density access");
    expectTrue(!fuse::renderer::froxel_util::canAccessDensityGrid(grid, zeroDesc),
               "empty froxel desc denies density access");

    expectTrue(fuse::renderer::froxel_util::writeDensityAtIndex(grid, desc, 5u, 0.75f),
               "write succeeds when access granted");
    expectNear(fuse::renderer::froxel_util::sampleDensityAtIndex(grid, desc, 5u), 0.75f, 1e-5f,
               "sample reads written density");
               "partially written grid still validates grid density");

    fuse::renderer::FroxelDensityGrid zeroAllocated{};
    zeroAllocated.allocate(zeroDesc);
    expectTrue(zeroAllocated.isEmpty(), "allocate on empty desc stays empty");
    expectTrue(fuse::renderer::froxel_util::validateGridDensity(zeroAllocated, zeroDesc),
               "empty allocated grid validates against empty desc");
    expectTrue(!fuse::renderer::froxel_util::canAccessDensityGrid(zeroAllocated, desc),
               "empty allocated grid cannot access non-empty desc");


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

    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroDesc, camera, params),
               "empty desc skips populate fill");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(zeroDesc, camera, params, populateReason),
               "empty desc fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::EmptyDesc,
               "empty desc reports empty_desc populate reject reason");

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

    grid.density[desc.maxFroxelIndex()] = 2.f;

    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 0u, lookupReason),
               "origin index lookup preflight succeeds");
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

    expectTrue(fuse::renderer::FroxelGridLayout::canPreflightSampleCoords(inBounds, desc),
               "canPreflightSampleCoords succeeds for in-bounds coords");
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(inBounds, desc, sampleReason),
               "tryPreflightSampleCoords succeeds for in-bounds coords");
               "in-bounds coords report no sample preflight reject reason");
    expectTrue(!fuse::renderer::FroxelGridLayout::wouldClampSampleCoords(inBounds, desc),
               "in-bounds coords would not clamp");

    fuse::renderer::FroxelSampleCoords warnWeights = inBounds;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(warnWeights, desc, sampleReason),
               "tryPreflightSampleCoords warns but succeeds for clampable weights");
               "clampable weights report invalid_weights sample preflight reason");
    expectTrue(fuse::renderer::FroxelGridLayout::wouldClampSampleCoords(warnWeights, desc),
               "clampable weights would clamp before sampling");

    fuse::renderer::FroxelSampleCoords hardOob = inBounds;
    hardOob.tileX0 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightSampleCoords(hardOob, desc, sampleReason),
               "tryPreflightSampleCoords rejects hard OOB tile coord");
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
               "trySampleDensityBilinear with reason matches unguarded sample");

    fuse::f32 rejectedBilinear = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, hardOob, rejectedBilinear, sampleReason),
               "trySampleDensityBilinear rejects hard OOB coords");
    expectNear(rejectedBilinear, 0.f, 1e-6f, "trySampleDensityBilinear zeroes output on hard OOB rejection");
    expectTrue(fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, hardOob) >= 0.f,
               "unguarded bilinear sample still clamps hard OOB coords");

void testFroxelTrilinearAndWouldSkipGuards() {


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

    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookup(emptyGrid, desc),
               "empty storage skips density lookup");
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookup(emptyGrid, desc, 0u),
               "empty storage skips index density lookup");
    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u),
               "empty storage skips coord density lookup");

    expectTrue(fuse::renderer::froxel_util::wouldSkipDensityLookup(grid, zeroDesc),
               "empty froxel desc skips density lookup");


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

    expectTrue(fuse::renderer::froxel_util::tryCanTrilinearSampleAtCoords(grid, desc, warnWeights, trilinearReason),
               "tryCanTrilinearSampleAtCoords warns but succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::ClampableWeights,
               "clampable weights report clampable_weights trilinear reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelTrilinearSampleRejectReasonLabel(trilinearReason),
                           "clampable_weights") == 0,
               "clampable_weights trilinear reject reason label");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipDensityTrilinearSample(grid, desc, warnWeights),
               "clampable weights do not skip trilinear sample");

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
                           "inaccessible_grid") == 0,
               "inaccessible_grid trilinear reject reason label");

    expectTrue(!fuse::renderer::froxel_util::tryCanTrilinearSampleAtCoords(grid, zeroDesc, inBounds, trilinearReason),
               "tryCanTrilinearSampleAtCoords rejects empty froxel desc");
    expectTrue(trilinearReason == fuse::renderer::FroxelTrilinearSampleRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid trilinear reject reason");

    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, inBounds, trilinearSample, trilinearReason),
               "trySampleDensityTrilinear with trilinear reason succeeds on accessible grid");
               "trySampleDensityTrilinear with trilinear reason matches unguarded sample");
               "successful trilinear sample reports no trilinear reject reason");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, hardOob, rejectedTrilinear, trilinearReason),
               "trySampleDensityTrilinear with trilinear reason rejects hard OOB coords");
    expectNear(rejectedTrilinear, 0.f, 1e-6f, "trySampleDensityTrilinear with trilinear reason zeroes output on rejection");
               "rejected trilinear sample reports invalid_sample_coords reason");



    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, params),
               "valid populate inputs do not skip froxel populate");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(zeroDesc, camera, params),
               "empty desc skips froxel populate");

    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroDensity),
               "zero density skips froxel populate");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroDensity) ==
                   fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, camera, zeroDensity),
               "wouldSkipFroxelPopulate agrees with shouldSkipFroxelPopulate for zero density");

    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroMarch),
               "zero march steps skip froxel populate");
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, camera, zeroMarch) ==
                   fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, camera, zeroMarch),
               "wouldSkipFroxelPopulate agrees with shouldSkipFroxelPopulate for zero march steps");

    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelPopulate(desc, badCamera, params),
               "invalid camera skips froxel populate");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, badCamera, params),
               "shouldSkipFroxelPopulate ignores invalid camera");

void testFroxelCoordLookupAndDiagnosticGuards() {

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

    expectTrue(fuse::renderer::FroxelGridLayout::isCoordOutOfRange(0u, 0u, 0u, zeroDesc),
               "any coord is out of range on empty grid");


    expectTrue(fuse::renderer::froxel_util::canLookupAtCoord(grid, desc, 1u, 1u, 2u),
               "accessible grid passes coord lookup preflight");
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, desc, 1u, 1u, 2u, lookupReason),
               "tryCanLookupAtCoord succeeds on accessible grid");
               "in-range coords report no lookup reject reason");

    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, desc, 99u, 99u, 99u, lookupReason),
               "OOB coord lookup preflight still succeeds with clamp warning");
               "OOB coords report index_out_of_range lookup reject reason");
    expectTrue(fuse::renderer::froxel_util::wouldClampDensityLookupCoord(99u, 99u, 99u, desc),
               "wouldClampDensityLookupCoord true for OOB coords");
    expectTrue(!fuse::renderer::froxel_util::wouldClampDensityLookupCoord(3u, 1u, 2u, desc),
               "wouldClampDensityLookupCoord false for last valid coords");

    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u, lookupReason),
               "tryCanLookupAtCoord rejects empty storage");
               "empty storage reports empty_storage lookup reject reason");

    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordSample, lookupReason),
               "trySampleDensityAtCoord with reason succeeds on accessible grid");
    expectNear(coordSample, 1.f, 1e-5f, "trySampleDensityAtCoord with reason returns origin density");
               "successful coord sample reports no lookup reject reason");

    fuse::f32 oobCoordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 99u, 99u, 99u, oobCoordSample, lookupReason),
               "trySampleDensityAtCoord with reason clamps OOB coords");
    expectNear(oobCoordSample, 2.f, 1e-5f, "trySampleDensityAtCoord with reason clamps to last cell");
               "OOB coord sample reports index_out_of_range lookup reject reason");

    fuse::f32 indexSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 999u, indexSample, lookupReason),
               "trySampleDensityAtIndex with reason clamps OOB index");
    expectNear(indexSample, 2.f, 1e-5f, "trySampleDensityAtIndex with reason returns clamped density");
               "OOB index sample reports index_out_of_range lookup reject reason");

    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 3.75f, lookupReason),
               "tryWriteDensityAtCoord with reason succeeds on accessible grid");
               "successful coord write reports no lookup reject reason");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 4.25f, lookupReason),
               "tryWriteDensityAtIndex with reason succeeds on accessible grid");
               "successful index write reports no lookup reject reason");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(emptyGrid, desc, 0u, 1.f, lookupReason),
               "tryWriteDensityAtIndex with reason rejects empty storage");
               "rejected index write reports empty_storage lookup reject reason");

                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, mapReason),
               "trySampleDensityAtScreen with reason succeeds on accessible grid");
               "successful screen sample reports no screen-mapping reject reason");
               "trySampleDensityAtScreen with reason matches unguarded sample");

                   emptyGrid, desc, camera, 0.5f, 0.5f, 10.f, rejectedScreen, mapReason),
               "trySampleDensityAtScreen with reason rejects empty storage");
               "empty storage maps to empty_grid screen-mapping reject reason");
    expectNear(rejectedScreen, 0.f, 1e-6f, "trySampleDensityAtScreen with reason zeroes output on rejection");

                   grid, desc, camera, 0.5f, 0.5f, 0.01f, rejectedScreen, mapReason),
               "trySampleDensityAtScreen with reason rejects depth below near plane");
               "below-near depth reports depth_out_of_range screen-mapping reject reason");

    fuse::renderer::FroxelDensityGrid populated{};
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, desc, camera, params, populateReason),
               "tryPopulate with reason succeeds for valid inputs");
               "successful populate reports no reject reason");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(populated) == desc.froxelCount(),
               "tryPopulate with reason fills all froxels on success");

    fuse::renderer::FroxelDensityGrid skipped{};
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(skipped, desc, camera, zeroDensity, populateReason),
               "tryPopulate with reason returns false when preflight rejects fill");
               "rejected populate reports zero_density reject reason");
    expectTrue(skipped.matchesDesc(desc), "tryPopulate with reason still allocates on rejected fill");

void testFroxelClassifyPreflightAndIsBlockingGuards() {



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

    expectTrue(fuse::renderer::FroxelGridLayout::preflightScreenMapping(
                   0.5f, 0.5f, 10.f, desc, camera, &mapped),
               "preflightScreenMapping succeeds in range");
    expectTrue(mapped.tileX0 < desc.tilesX && mapped.sliceZ0 < desc.slicesZ,
               "preflightScreenMapping returns mapped coords");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenMappingReject(0.5f, 0.5f, 10.f, desc, camera) ==
                   fuse::renderer::ScreenMappingRejectReason::None,
               "classifyScreenMappingReject none for valid mapping");

    expectTrue(!fuse::renderer::FroxelGridLayout::preflightScreenMapping(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, nullptr, &mapReason),
               "preflightScreenMapping rejects empty grid");
               "preflightScreenMapping reports empty_grid reject reason");
    expectTrue(fuse::renderer::FroxelGridLayout::classifyScreenMappingReject(0.5f, 0.5f, 10.f, zeroDesc, camera) ==
                   fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "classifyScreenMappingReject empty_grid for empty desc");

    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(inBounds, desc, &sampleReason),
               "preflightSampleCoords succeeds for in-bounds coords");
               "preflightSampleCoords reports no reject reason for in-bounds coords");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordReject(inBounds, desc) ==
                   fuse::renderer::SampleCoordRejectReason::None,
               "classifySampleCoordReject none for in-bounds coords");

    expectTrue(fuse::renderer::FroxelGridLayout::preflightSampleCoords(warnWeights, desc, &sampleReason),
               "preflightSampleCoords succeeds for clampable weights");
               "preflightSampleCoords reports invalid_weights for clampable weights");
    expectTrue(fuse::renderer::FroxelGridLayout::classifySampleCoordReject(warnWeights, desc) ==
                   fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "classifySampleCoordReject invalid_weights for clampable weights");

    expectTrue(!fuse::renderer::FroxelGridLayout::preflightSampleCoords(hardOob, desc, &sampleReason),
               "preflightSampleCoords rejects hard OOB tile coord");
               "preflightSampleCoords reports out_of_bounds for hard OOB tile coord");


    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 0u, &lookupReason),
               "preflightDensityLookup succeeds for accessible grid");
               "preflightDensityLookup reports no reject reason for in-range index");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(grid, desc, 0u) ==
                   fuse::renderer::DensityLookupRejectReason::None,
               "classifyDensityLookupReject none for in-range index");

    expectTrue(fuse::renderer::froxel_util::preflightDensityLookup(grid, desc, 999u, &lookupReason),
               "preflightDensityLookup succeeds for OOB index that clamps");
               "preflightDensityLookup reports index_out_of_range for OOB index");
    expectTrue(fuse::renderer::froxel_util::preflightDensityLookupAtCoord(grid, desc, 99u, 99u, 99u, &lookupReason),
               "preflightDensityLookupAtCoord succeeds for OOB coords that clamp");
               "preflightDensityLookupAtCoord reports index_out_of_range for OOB coords");

    expectTrue(!fuse::renderer::froxel_util::preflightDensityLookup(emptyGrid, desc, 0u, &lookupReason),
               "preflightDensityLookup rejects empty storage");
               "preflightDensityLookup reports empty_storage for empty grid");
    expectTrue(fuse::renderer::froxel_util::classifyDensityLookupReject(emptyGrid, desc, 0u) ==
                   fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "classifyDensityLookupReject empty_storage for empty grid");

    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, inBounds, &trilinearReason),
               "preflightTrilinearSample succeeds for in-bounds coords");
               "preflightTrilinearSample reports no reject reason for in-bounds coords");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelTrilinearSampleReject(grid, desc, inBounds) ==
                   fuse::renderer::FroxelTrilinearSampleRejectReason::None,
               "classifyFroxelTrilinearSampleReject none for in-bounds coords");

    expectTrue(fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, warnWeights, &trilinearReason),
               "preflightTrilinearSample succeeds for clampable weights");
               "preflightTrilinearSample reports clampable_weights for OOB weights");
    expectTrue(!fuse::renderer::froxel_util::preflightTrilinearSample(grid, desc, hardOob, &trilinearReason),
               "preflightTrilinearSample rejects hard OOB tile coord");
               "preflightTrilinearSample reports invalid_sample_coords for hard OOB tile coord");

    expectTrue(fuse::renderer::froxel_util::preflightGridDensity(grid, desc, &densityReason),
               "preflightGridDensity succeeds for accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "preflightGridDensity reports no reject reason for accessible grid");
    expectTrue(fuse::renderer::froxel_util::classifyGridDensityReject(grid, desc) ==
                   fuse::renderer::GridDensityRejectReason::None,
               "classifyGridDensityReject none for accessible grid");

    expectTrue(!fuse::renderer::froxel_util::preflightGridDensity(undersized, desc, &densityReason),
               "preflightGridDensity rejects undersized storage");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "preflightGridDensity reports undersized_storage reject reason");
    expectTrue(fuse::renderer::froxel_util::classifyGridDensityReject(undersized, desc) ==
                   fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "classifyGridDensityReject undersized_storage for undersized storage");

    expectTrue(fuse::renderer::froxel_util::preflightFroxelPopulate(desc, camera, params, &populateReason),
               "preflightFroxelPopulate succeeds for valid inputs");
               "preflightFroxelPopulate reports no reject reason for valid inputs");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelPopulateReject(desc, camera, params) ==
                   fuse::renderer::FroxelPopulateRejectReason::None,
               "classifyFroxelPopulateReject none for valid inputs");

    expectTrue(!fuse::renderer::froxel_util::preflightFroxelPopulate(desc, camera, zeroDensity, &populateReason),
               "preflightFroxelPopulate rejects zero density");
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
    expectNear(fuse::renderer::froxel_util::sampleDensityAtScreen(grid, zeroDesc, camera, 0.5f, 0.5f, 10.f),
               0.f,
               1e-6f,
               "screen sample early-outs on empty froxel desc");
    expectTrue(!fuse::renderer::froxel_util::writeDensityAtIndex(grid, mismatched, 0u, 1.f),
               "write guard rejects desc mismatch via canAccessDensityGrid");
    grid.density[0] = 1.25f;
    grid.density[desc.maxFroxelIndex()] = 2.75f;

    expectTrue(fuse::renderer::froxel_util::canAccessDensityAtIndex(grid, desc, 0u),
               "can access density at origin index");
    expectTrue(fuse::renderer::froxel_util::canAccessDensityAtIndex(grid, desc, 999u),
               "can access density at OOB index when grid matches desc");
    expectTrue(!fuse::renderer::froxel_util::canAccessDensityAtIndex(grid, zeroDesc, 0u),
               "can access rejects empty froxel desc");

    expectTrue(!fuse::renderer::froxel_util::canAccessDensityAtIndex(grid, mismatched, 0u),
               "can access rejects desc mismatch");

    expectTrue(!fuse::renderer::froxel_util::canAccessDensityAtIndex(emptyGrid, desc, 0u),
               "can access rejects empty density storage");

    fuse::f32 sampledDensity = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampledDensity),
               "try sample density succeeds at origin");
    expectNear(sampledDensity, 1.25f, 1e-5f, "try sample returns stored density");

    fuse::f32 rejectedSample = 99.f;
               "try sample rejects empty storage");
    expectNear(rejectedSample, 0.f, 1e-6f, "try sample zeroes output on rejection");

    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 3.5f),
               "try write density succeeds at in-bounds index");
    fuse::f32 writtenDensity = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 5u, writtenDensity),
               "try write is readable via try sample");
    expectNear(writtenDensity, 3.5f, 1e-5f, "try write stores requested density");

               "try write rejects desc mismatch");
               "try write rejects empty storage");

    fuse::renderer::DensityGridRejectReason reason = fuse::renderer::DensityGridRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateDensityCounts(grid, desc, reason),
               "try validate accepts populated grid");
    expectTrue(reason == fuse::renderer::DensityGridRejectReason::None, "valid grid reports no reject reason");
    expectTrue(fuse::renderer::froxel_util::validateDensityCountsForDesc(grid, desc),
               "validate for desc accepts populated grid");

    expectTrue(fuse::renderer::froxel_util::tryValidateDensityCounts(emptyGrid, zeroDesc, reason),
               "try validate vacuously accepts empty froxel desc");
    expectTrue(reason == fuse::renderer::DensityGridRejectReason::None,
               "empty froxel desc reports no reject reason");

    expectTrue(!fuse::renderer::froxel_util::tryValidateDensityCounts(emptyGrid, desc, reason),
               "try validate rejects empty storage for non-empty desc");
    expectTrue(reason == fuse::renderer::DensityGridRejectReason::EmptyGridDesc,
               "empty storage reports empty grid desc reason");
    expectTrue(std::string(fuse::renderer::densityGridRejectReasonLabel(reason)) == "empty_grid_desc",
               "empty grid desc reason label");

    expectTrue(!fuse::renderer::froxel_util::tryValidateDensityCounts(grid, mismatched, reason),
               "try validate rejects desc mismatch");
    expectTrue(reason == fuse::renderer::DensityGridRejectReason::DescMismatch,
               "desc mismatch reports correct reason");
    expectTrue(std::string(fuse::renderer::densityGridRejectReasonLabel(reason)) == "desc_mismatch",
               "desc mismatch reason label");

    expectTrue(fuse::renderer::froxel_util::validateDensityCountsForDesc(emptyGrid, zeroDesc),
               "validate for desc vacuously accepts empty froxel desc");
    inBounds.ty = 0.25f;
               "in-bounds sample coords accepted");
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

    fuse::renderer::FroxelSampleCoords emptyClamp = oobWeights;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(emptyClamp, zeroDesc),
               "tryClamp sample coords rejects empty grid");
    expectTrue(!fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(inBounds, zeroDesc),
               "in-bounds coords rejected on empty grid desc");

void testFroxelDensityAccessAndValidationDeepen() {

    grid.density[0] = 1.5f;
    grid.density[23] = 2.5f;

    expectTrue(fuse::renderer::froxel_util::canSampleDensityAtIndex(grid, desc, 0u),
               "canSample accepts matching grid and desc");
    expectTrue(fuse::renderer::froxel_util::canSampleDensityAtIndex(grid, desc, 999u),
               "canSample accepts OOB index that will be clamped");

    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled),
               "trySample succeeds on valid grid");
    expectNear(sampled, 1.5f, 1e-5f, "trySample returns origin density");

    fuse::f32 clampedSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 999u, clampedSample),
               "trySample succeeds when clamping OOB index");
    expectNear(clampedSample, 2.5f, 1e-5f, "trySample clamps OOB index to last froxel");

    fuse::f32 rejectedSample = 9.f;
    expectNear(rejectedSample, 0.f, 1e-6f, "trySample zeroes output on guard failure");

    grid.density[23] = 2.75f;

    expectTrue(fuse::renderer::froxel_util::canSampleAtIndex(grid, desc, 0u),
    expectTrue(fuse::renderer::froxel_util::canSampleAtIndex(grid, desc, 999u),
    expectTrue(fuse::renderer::froxel_util::canSampleAtCoord(grid, desc),
               "canSampleAtCoord accepts matching grid and desc");

               "trySample at origin succeeds");
    expectNear(sampledDensity, 1.25f, 1e-5f, "trySample at origin returns stored density");

    fuse::f32 clampedDensity = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 999u, clampedDensity),
               "trySample at OOB index succeeds");
    expectNear(clampedDensity, 2.75f, 1e-5f, "trySample at clamped last index returns stored density");

    fuse::f32 coordDensity = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordDensity),
               "trySample at tile coords succeeds");
    expectNear(coordDensity, 1.25f, 1e-5f, "trySample at tile coords returns stored density");

    fuse::f32 rejectedDensity = 9.f;
    expectTrue(!fuse::renderer::froxel_util::canSampleAtIndex(emptyGrid, desc, 0u),
               "canSample rejects empty storage");
    expectTrue(!fuse::renderer::froxel_util::canSampleAtCoord(emptyGrid, desc),
               "canSampleAtCoord rejects empty storage");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedDensity),
    expectNear(rejectedDensity, 0.f, 1e-6f, "trySample zeroes output on guard failure");
    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);

               "accessible grid does not skip lookup");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc),
               "non-empty desc does not skip populate");

    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroDesc),
               "empty desc skips populate");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelLookup(grid, zeroDesc),
               "empty desc skips lookup");

    fuse::renderer::FroxelLookupRejectReason lookupReason = fuse::renderer::FroxelLookupRejectReason::None;
               "tryCanLookup accepts accessible grid");
    expectTrue(lookupReason == fuse::renderer::FroxelLookupRejectReason::None,

               "tryCanLookup rejects empty grid desc");
    expectTrue(lookupReason == fuse::renderer::FroxelLookupRejectReason::EmptyGrid,
               "empty grid desc reports empty_grid reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelLookupRejectReasonLabel(lookupReason), "empty_grid") == 0,
               "empty grid lookup reject label");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
               "tryCanLookup rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::FroxelLookupRejectReason::EmptyStorage,

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

    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(oversized, desc, reason),
               "tryValidate rejects oversized storage");
               "oversized storage reports desc_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::froxelDensityRejectReasonLabel(reason), "desc_mismatch") == 0,
               "reject reason label for desc mismatch");

    expectTrue(!fuse::renderer::froxel_util::validateGridDensityForDesc(grid, mismatched),
               "validateGridDensityForDesc rejects desc mismatch");
    expectTrue(!fuse::renderer::froxel_util::canSampleAtIndex(grid, mismatched, 0u),
    expectTrue(!fuse::renderer::froxel_util::canSampleAtCoord(grid, mismatched),
               "canSampleAtCoord rejects desc mismatch");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, mismatched, 0u, 0u, 0u, coordDensity),
               "trySample at coord rejects desc mismatch");

    fuse::renderer::FroxelSampleCoords unclampedCoords{};
    unclampedCoords.tileX0 = 99u;
    unclampedCoords.tileY0 = 99u;
    unclampedCoords.tileX1 = 99u;
    unclampedCoords.tileY1 = 99u;
    unclampedCoords.sliceZ0 = 99u;
    unclampedCoords.sliceZ1 = 99u;
    unclampedCoords.tx = 2.f;
    unclampedCoords.ty = -1.f;
    unclampedCoords.tz = 3.f;
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(unclampedCoords, desc),
               "tryClamp sample coords succeeds on non-empty grid");
    expectTrue(unclampedCoords.tileX0 == 3u && unclampedCoords.tileY0 == 1u && unclampedCoords.sliceZ0 == 2u,
               "tryClamp sample coords clamps tile/slice corners");

    fuse::renderer::FroxelSampleCoords emptyCoords{};
    emptyCoords.tileX0 = 99u;
    emptyCoords.tileY0 = 99u;
    emptyCoords.tileX1 = 99u;
    emptyCoords.tileY1 = 99u;
    emptyCoords.sliceZ0 = 99u;
    emptyCoords.sliceZ1 = 99u;
    emptyCoords.tx = 2.f;
    emptyCoords.ty = -1.f;
    emptyCoords.tz = 3.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(emptyCoords, zeroDesc),
               "tryClamp sample coords rejects empty grid");
    expectTrue(emptyCoords.tileX0 == 99u, "tryClamp leaves coords untouched on empty grid");
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, mismatched, 0u, lookupReason),
               "tryCanLookup rejects desc mismatch");
    expectTrue(lookupReason == fuse::renderer::FroxelLookupRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reject reason");

    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 3u, 1.25f, lookupReason),
               "tryWrite with reject reason succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 3u, sampled, lookupReason),
               "trySample with reject reason succeeds on accessible grid");
    expectNear(sampled, 1.25f, 1e-5f, "trySample with reject reason returns written density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, sampled, lookupReason),
               "trySample with reject reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::FroxelLookupRejectReason::EmptyStorage,
               "trySample reports empty_storage reject reason");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleRejectReason sampleReason = fuse::renderer::FroxelSampleRejectReason::None;
    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, desc, camera, mapped, sampleReason),
               "tryMap sample coords succeeds in range");
    expectTrue(sampleReason == fuse::renderer::FroxelSampleRejectReason::None,
               "successful mapping reports no sample reject reason");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, mapped, sampleReason),
               "tryMap rejects depth below near plane");
    expectTrue(sampleReason == fuse::renderer::FroxelSampleRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range");
    expectTrue(std::strcmp(fuse::renderer::froxelSampleRejectReasonLabel(sampleReason), "depth_out_of_range") == 0,
               "depth out of range sample reject label");

    fuse::u32 froxelIndex = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 200.f, desc, camera, froxelIndex, sampleReason),
               "tryMap froxel index rejects depth above far plane");
               "above-far depth reports depth_out_of_range for index mapping");

    fuse::renderer::FroxelCameraDesc invalidCamera{};
    invalidCamera.nearPlane = 0.f;
    invalidCamera.farPlane = 100.f;
                   0.5f, 0.5f, 10.f, desc, invalidCamera, mapped, sampleReason),
               "tryMap rejects invalid camera");
    expectTrue(sampleReason == fuse::renderer::FroxelSampleRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera reject reason");

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
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(extremeCoords, desc),
    expectTrue(extremeCoords.tileX0 == 3u && extremeCoords.tileY0 == 1u && extremeCoords.sliceZ0 == 2u,
               "tryClamp clamps tile/slice corners");

               "tryClamp rejects empty grid");

    fuse::f32 screenDensity = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenDensity, sampleReason),
               "trySample at screen succeeds on accessible grid");
    expectNear(screenDensity,
               fuse::renderer::froxel_util::sampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               1e-5f,
               "trySample at screen matches legacy sample");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   emptyGrid, desc, camera, 0.5f, 0.5f, 10.f, screenDensity, sampleReason),
               "trySample at screen rejects empty storage");
    expectTrue(sampleReason == fuse::renderer::FroxelSampleRejectReason::InaccessibleGrid,
               "empty storage screen sample reports inaccessible_grid");

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
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, 0u, desc)] = 0.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 0u, 0u, desc)] = 1.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 1u, 0u, desc)] = 0.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 1u, 0u, desc)] = 1.f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 0u, 1u, desc)] = 0.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 0u, 1u, desc)] = 1.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(0u, 1u, 1u, desc)] = 0.5f;
    grid.density[fuse::renderer::FroxelGridLayout::froxelIndex(1u, 1u, 1u, desc)] = 1.5f;

    fuse::f32 trilinear = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, coords, trilinear),
               "trySample trilinear succeeds on accessible grid");
    expectNear(trilinear, fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, coords), 1e-5f,
               "trySample trilinear matches legacy sample");

    fuse::f32 bilinear = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, coords, bilinear),
               "trySample bilinear succeeds on accessible grid");
    expectNear(bilinear, fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, coords), 1e-5f,
               "trySample bilinear matches legacy sample");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(emptyGrid, desc, coords, trilinear),
               "trySample trilinear rejects empty storage");
}

void testFroxelSampleCoordRejectAndEmptyGridGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    expectTrue(!fuse::renderer::froxel_util::isEmptyGridForSampling(grid, desc),
               "allocated grid is sampleable");

    fuse::renderer::FroxelGridDesc emptyDesc{};
    emptyDesc.tilesX = 0u;
    expectTrue(fuse::renderer::froxel_util::isEmptyGridForSampling(grid, emptyDesc),
               "zero-dimension desc is empty for sampling");

    fuse::renderer::FroxelDensityGrid emptyStorage{};
    expectTrue(fuse::renderer::froxel_util::isEmptyGridForSampling(emptyStorage, desc),
               "empty storage is empty for sampling");

    fuse::renderer::FroxelSampleCoords inBounds{};
    inBounds.tileX0 = 0u;
    inBounds.tileY0 = 0u;
    inBounds.tileX1 = 1u;
    inBounds.tileY1 = 1u;
    inBounds.sliceZ0 = 0u;
    inBounds.sliceZ1 = 1u;
    inBounds.tx = 0.25f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.5f;
    expectTrue(fuse::renderer::froxel_util::canSampleAtCoords(grid, desc, inBounds),
               "in-bounds coords pass sample preflight");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipDensitySample(grid, desc, inBounds),
               "in-bounds coords do not skip density sample");

    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, inBounds, coordReason),
               "tryCanSampleAtCoords succeeds on accessible grid");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "accessible grid reports no sample-coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "none") == 0,
               "none sample-coord reject reason label");

    fuse::renderer::FroxelSampleCoords outOfRange = inBounds;
    outOfRange.tileX0 = 99u;
    expectTrue(!fuse::renderer::froxel_util::canSampleAtCoords(grid, desc, outOfRange),
               "OOB tile coord fails sample preflight");
    expectTrue(fuse::renderer::froxel_util::shouldSkipDensitySample(grid, desc, outOfRange),
               "OOB coords skip density sample");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, outOfRange, coordReason),
               "tryCanSampleAtCoords rejects OOB coords");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRange,
               "OOB coords report out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "out_of_range") == 0,
               "out_of_range sample-coord reject reason label");

    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.25f, 0.25f, 3.16f, desc, camera, mapped, coordReason),
               "tryMapScreenDepthToSampleCoords succeeds in frustum");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "successful screen mapping reports no reject reason");

    fuse::renderer::FroxelSampleCoords rejectedDepth{};
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, rejectedDepth, coordReason),
               "tryMapScreenDepthToSampleCoords rejects depth below near plane");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "depth_out_of_range") == 0,
               "depth_out_of_range sample-coord reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::FroxelSampleCoords zeroCoords{};
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, zeroCoords, coordReason),
               "tryMapScreenDepthToSampleCoords rejects empty froxel desc");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid sample-coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "empty_grid") == 0,
               "empty_grid sample-coord reject reason label");

    fuse::renderer::FroxelSampleCoords clamped = outOfRange;
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(clamped, desc, coordReason),
               "tryClampSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(clamped, desc),
               "tryClampSampleCoords produces in-bounds coords");

    fuse::f32 bilinearWithReason = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, bilinearWithReason, coordReason),
               "trySampleDensityBilinear with reason succeeds on accessible grid");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "successful bilinear sample reports no reject reason");

    fuse::f32 rejectedBilinear = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(
                   emptyStorage, desc, inBounds, rejectedBilinear, coordReason),
               "trySampleDensityBilinear with reason rejects empty storage");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty storage bilinear sample reports empty_grid reject reason");
    expectNear(rejectedBilinear, 0.f, 1e-6f, "rejected bilinear sample zeroes output");

    fuse::f32 indexSample = 0.f;
    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, indexSample, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectNear(indexSample, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns stored density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index sample reports no lookup reject reason");

    fuse::f32 rejectedIndex = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(
                   emptyStorage, desc, 0u, rejectedIndex, lookupReason),
               "trySampleDensityAtIndex with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage index sample reports empty_storage lookup reject reason");
    expectNear(rejectedIndex, 0.f, 1e-6f, "rejected index sample zeroes output");

    fuse::f32 screenWithReason = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenWithReason, coordReason),
               "trySampleDensityAtScreen with reason succeeds on accessible grid");
    expectNear(screenWithReason,
               fuse::renderer::froxel_util::sampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               1e-5f,
               "trySampleDensityAtScreen with reason matches unguarded sample");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   emptyStorage, desc, camera, 0.5f, 0.5f, 10.f, rejectedScreen, coordReason),
               "trySampleDensityAtScreen with reason rejects empty storage");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty storage screen sample reports empty_grid reject reason");

    fuse::renderer::FroxelDensityGrid oversized{};
    oversized.density.resize(desc.froxelCount() + 1u, 0.f);
    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(oversized, desc, densityReason),
               "oversized storage fails grid density validation");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DensityCountMismatch,
               "oversized storage reports density_count_mismatch reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "density_count_mismatch") == 0,
               "density_count_mismatch grid reject reason label");
}

void testFroxelSampleCoordRejectAndPopulateGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(fuse::renderer::froxel_util::canSampleFroxelGrid(desc),
               "non-empty froxel desc is sampleable");
    expectTrue(fuse::renderer::froxel_util::canPopulateFroxelGrid(desc),
               "non-empty froxel desc can populate");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::froxel_util::canSampleFroxelGrid(zeroDesc),
               "empty froxel desc is not sampleable");
    expectTrue(!fuse::renderer::froxel_util::canPopulateFroxelGrid(zeroDesc),
               "empty froxel desc cannot populate");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    expectTrue(fuse::renderer::FroxelGridLayout::canMapScreenDepth(10.f, camera),
               "mid-depth is mapable");
    expectTrue(!fuse::renderer::FroxelGridLayout::canMapScreenDepth(0.01f, camera),
               "depth below near plane is not mapable");
    expectTrue(!fuse::renderer::FroxelGridLayout::canMapScreenDepth(200.f, camera),
               "depth above far plane is not mapable");

    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, desc, camera, mapped, coordReason),
               "tryMap sample coords succeeds on valid request");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "successful map reports no coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "none") == 0,
               "none sample coord reject label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, mapped, coordReason),
               "tryMap sample coords rejects empty grid");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty grid reports empty_grid coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "empty_grid") == 0,
               "empty_grid sample coord reject label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, mapped, coordReason),
               "tryMap sample coords rejects invalid depth");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::InvalidDepth,
               "invalid depth reports invalid_depth coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "invalid_depth") == 0,
               "invalid_depth sample coord reject label");

    fuse::u32 froxelIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, desc, camera, froxelIndex, coordReason),
               "tryMap froxel index succeeds on valid request");
    expectTrue(froxelIndex < desc.froxelCount(), "tryMap froxel index in bounds");

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
    expectTrue(fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(inBounds, desc, coordReason),
               "tryAreSampleCoordsInBounds accepts in-bounds coords");

    fuse::renderer::FroxelSampleCoords outOfBounds = inBounds;
    outOfBounds.tileX0 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(outOfBounds, desc, coordReason),
               "tryAreSampleCoordsInBounds rejects OOB tile coord");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "OOB coords report out_of_bounds reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "out_of_bounds") == 0,
               "out_of_bounds sample coord reject label");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    expectTrue(fuse::renderer::froxel_util::canLookupAtCoord(grid, desc, 1u, 1u, 2u),
               "canLookupAtCoord accepts accessible grid");

    fuse::renderer::DensityLookupRejectReason coordLookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtCoord(grid, desc, 1u, 1u, 2u, coordLookupReason),
               "tryCanLookupAtCoord accepts accessible grid at coord");
    expectTrue(coordLookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "accessible coord lookup reports no reject reason");
}

void testFroxelDensityLookupRejectAndValidationGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;

    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelSample(grid, desc),
               "partially filled grid does not skip sample");
    expectTrue(fuse::renderer::froxel_util::isValidDensitySampleRequest(grid, desc, fuse::renderer::FroxelSampleCoords{}),
               "accessible grid accepts clampable sample coords");

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled, lookupReason),
               "trySample at index with reason succeeds");
    expectNear(sampled, 1.f, 1e-5f, "trySample at index with reason returns density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index sample clears lookup reject reason");

    fuse::renderer::FroxelSampleCoords coords{};
    coords.tileX0 = 0u;
    coords.tileY0 = 0u;
    coords.tileX1 = 1u;
    coords.tileY1 = 1u;
    coords.sliceZ0 = 0u;
    coords.sliceZ1 = 1u;
    fuse::f32 bilinear = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, coords, bilinear, lookupReason),
               "trySample bilinear with reason succeeds");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful bilinear sample clears lookup reject reason");

    fuse::f32 trilinear = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, coords, trilinear, lookupReason),
               "trySample trilinear with reason succeeds");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::f32 screenSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, lookupReason),
               "trySample at screen with reason succeeds");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, sampled, lookupReason),
               "trySample at index with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage lookup reject reason");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "tryValidateGridDensityForDesc accepts accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "valid grid reports no density reject reason");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, mismatched, densityReason),
               "tryValidateGridDensityForDesc rejects desc mismatch");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch density reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "desc_mismatch") == 0,
               "desc_mismatch density reject label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    fuse::renderer::FroxelDensityGrid populated{};
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, desc, camera, params),
               "tryPopulate succeeds on non-empty desc");
    expectTrue(populated.matchesDesc(desc), "tryPopulate allocates matching grid");
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, zeroDesc, camera, params),
               "tryPopulate rejects empty froxel desc");
}

void testFroxelSampleCoordNormalizeAndBuildGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleCoords built{};
    expectTrue(fuse::renderer::FroxelGridLayout::buildFroxelSampleCoords(0.25f, 0.5f, 10.f, desc, camera, built),
               "buildFroxelSampleCoords succeeds on interior sample");
    expectTrue(fuse::renderer::FroxelGridLayout::isValidFroxelSampleCoords(built, desc),
               "built sample coords pass validity guard");

    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::mapScreenDepthToSampleCoords(
                   0.25f, 0.5f, 10.f, desc, camera, mapped),
               "mapScreenDepthToSampleCoords succeeds for comparison");
    expectTrue(mapped.tileX0 == built.tileX0 && mapped.tileY0 == built.tileY0 && mapped.sliceZ0 == built.sliceZ0,
               "buildFroxelSampleCoords matches mapScreenDepthToSampleCoords");

    fuse::renderer::FroxelSampleCoords reversed{};
    reversed.tileX0 = 3u;
    reversed.tileX1 = 1u;
    reversed.tileY0 = 1u;
    reversed.tileY1 = 0u;
    reversed.sliceZ0 = 2u;
    reversed.sliceZ1 = 0u;
    reversed.tx = 0.25f;
    reversed.ty = 0.75f;
    reversed.tz = 0.5f;
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidFroxelSampleCoords(reversed, desc),
               "reversed corner indices fail validity guard");
    fuse::renderer::FroxelGridLayout::normalizeFroxelSampleCoords(reversed);
    expectTrue(fuse::renderer::FroxelGridLayout::isValidFroxelSampleCoords(reversed, desc),
               "normalizeFroxelSampleCoords fixes reversed corners");
    expectTrue(reversed.tileX0 == 1u && reversed.tileX1 == 3u, "normalize swaps tile X corners into order");
    expectNear(reversed.tx, 0.75f, 1e-5f, "normalize inverts tx when tile X corners swap");

    fuse::renderer::FroxelSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    oobWeights.ty = -1.f;
    fuse::renderer::FroxelGridLayout::normalizeFroxelSampleCoords(oobWeights);
    expectTrue(fuse::renderer::FroxelGridLayout::isValidFroxelSampleCoords(oobWeights, desc),
               "normalize clamps OOB trilinear weights");

    fuse::renderer::FroxelSampleCoords oobIndices{};
    oobIndices.tileX0 = 99u;
    oobIndices.tileX1 = 99u;
    oobIndices.tileY0 = 99u;
    oobIndices.tileY1 = 99u;
    oobIndices.sliceZ0 = 99u;
    oobIndices.sliceZ1 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidFroxelSampleCoords(oobIndices, desc),
               "OOB corner indices fail validity guard before clamp");
    fuse::renderer::FroxelGridLayout::clampSampleCoords(oobIndices, desc);
    expectTrue(fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(oobIndices, desc),
               "clampSampleCoords yields in-bounds sample coords");

    fuse::renderer::FroxelGridDesc empty{};
    empty.tilesX = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::buildFroxelSampleCoords(0.5f, 0.5f, 10.f, empty, camera, built),
               "buildFroxelSampleCoords rejects empty grid");
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidFroxelSampleCoords(built, empty),
               "empty grid sample coords invalid");
}

void testFroxelDensityLookupBoundsAndPreflightGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[5u] = 2.5f;

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndexBounds(grid, desc, 5u, lookupReason),
               "index-bounds preflight accepts in-range index");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "in-range index reports no lookup reject reason");

    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 999u, lookupReason),
               "legacy lookup preflight still ignores OOB index");
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndexBounds(grid, desc, 999u, lookupReason),
               "index-bounds preflight rejects OOB index");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "OOB index reports index_out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "index_out_of_range") == 0,
               "index_out_of_range lookup reject reason label");

    fuse::f32 boundedSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndexBounds(grid, desc, 5u, boundedSample, lookupReason),
               "bounded sample succeeds on in-range index");
    expectNear(boundedSample, 2.5f, 1e-5f, "bounded sample returns stored density");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndexBounds(grid, desc, 999u, boundedSample, lookupReason),
               "bounded sample rejects OOB index");
    expectNear(boundedSample, 0.f, 1e-6f, "bounded sample zeroes output on rejection");

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
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtSampleCoords(grid, desc, inBounds, lookupReason),
               "sample-coord preflight accepts in-bounds coords");

    fuse::renderer::FroxelSampleCoords outOfRange = inBounds;
    outOfRange.tileX0 = 99u;
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtSampleCoords(grid, desc, outOfRange, lookupReason),
               "sample-coord preflight rejects OOB tile coord");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::SampleCoordsOutOfRange,
               "OOB sample coords report sample_coords_out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason),
                            "sample_coords_out_of_range") == 0,
               "sample_coords_out_of_range lookup reject reason label");

    fuse::f32 coordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinearAtCoords(
                   grid, desc, inBounds, coordSample, lookupReason),
               "trilinear coord-guarded sample succeeds on in-bounds coords");
    expectNear(coordSample,
               fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, inBounds),
               1e-5f,
               "trilinear coord-guarded sample matches unguarded sample");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinearAtCoords(
                   grid, desc, outOfRange, coordSample, lookupReason),
               "bilinear coord-guarded sample rejects OOB coords");
    expectNear(coordSample, 0.f, 1e-6f, "bilinear coord-guarded sample zeroes output on rejection");

    const fuse::renderer::FroxelGridPreflight preflight =
        fuse::renderer::froxel_util::preflightFroxelDensityGrid(grid, desc);
    expectTrue(preflight.desc_non_empty, "preflight sees non-empty desc");
    expectTrue(preflight.storage_allocated, "preflight sees allocated storage");
    expectTrue(preflight.storage_matches_desc, "preflight sees matching storage");
    expectTrue(preflight.density_counts_valid, "preflight sees valid density counts");
    expectTrue(preflight.can_lookup(), "accessible grid passes lookup preflight");
    expectTrue(preflight.can_validate(), "accessible grid passes validate preflight");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    const fuse::renderer::FroxelGridPreflight emptyPreflight =
        fuse::renderer::froxel_util::preflightFroxelDensityGrid(grid, zeroDesc);
    expectTrue(!emptyPreflight.desc_non_empty, "preflight marks empty desc");
    expectTrue(!emptyPreflight.can_lookup(), "empty desc fails lookup preflight");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityStrict(grid, zeroDesc, densityReason),
               "strict density validation rejects empty desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::EmptyDesc,
               "empty desc reports empty_desc reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "empty_desc") == 0,
               "empty_desc density reject reason label");
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensity(grid, zeroDesc, densityReason),
               "legacy density validation still vacuously succeeds on empty desc");

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.march_steps = 32u;
    const fuse::renderer::FroxelPopulatePreflight populatePreflight =
        fuse::renderer::froxel_util::preflightPopulateFroxelGrid(desc, params);
    expectTrue(populatePreflight.desc_non_empty, "populate preflight sees non-empty desc");
    expectTrue(populatePreflight.params_enabled, "populate preflight sees enabled params");
    expectTrue(populatePreflight.can_populate(), "populate preflight allows analytic populate");

    fuse::renderer::VolumetricFogParams disabled{};
    disabled.density = 0.f;
    const fuse::renderer::FroxelPopulatePreflight disabledPreflight =
        fuse::renderer::froxel_util::preflightPopulateFroxelGrid(desc, disabled);
    expectTrue(!disabledPreflight.params_enabled, "populate preflight rejects zero density");
    expectTrue(!disabledPreflight.can_populate(), "populate preflight blocks disabled params");
}

void testFroxelDensityValidationAndSampleGuardDeepen() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    expectTrue(fuse::renderer::froxel_util::writeDensityAtIndex(grid, desc, 0u, 1.f),
               "seed origin density for deepen guard tests");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "tryValidateGridDensityForDesc accepts accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "accessible grid reports no density reject reason");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, mismatched, densityReason),
               "tryValidateGridDensityForDesc rejects desc mismatch");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch density reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "desc_mismatch") == 0,
               "desc_mismatch density reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc vacuously succeeds on empty desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "empty desc validation reports no reject reason");

    fuse::renderer::FroxelDensityGrid oversized{};
    oversized.density.resize(desc.froxelCount() + 4u, 0.f);
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(oversized, desc, densityReason),
               "oversized storage fails density validation");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
               "oversized storage reports desc_mismatch reject reason");

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.march_steps = 32u;
    expectTrue(fuse::renderer::froxel_util::canPopulateFroxelGrid(desc, params),
               "canPopulate accepts non-empty desc with active fog params");
    expectTrue(!fuse::renderer::froxel_util::canPopulateFroxelGrid(zeroDesc, params),
               "canPopulate rejects empty froxel desc");
    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    expectTrue(!fuse::renderer::froxel_util::canPopulateFroxelGrid(desc, zeroDensity),
               "canPopulate rejects zero-density fog params");

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

    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateSampleCoords(inBounds, desc, coordReason),
               "tryValidateSampleCoords accepts in-bounds coords");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "in-bounds coords report no sample-coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "none") == 0,
               "none sample-coord reject reason label");

    fuse::renderer::FroxelSampleCoords outOfRange = inBounds;
    outOfRange.tx = 2.f;
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(outOfRange, desc, coordReason),
               "tryValidateSampleCoords rejects OOB interpolation weight");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRange,
               "OOB weight reports out_of_range sample-coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "out_of_range") == 0,
               "out_of_range sample-coord reject reason label");

    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(inBounds, zeroDesc, coordReason),
               "tryValidateSampleCoords rejects empty froxel desc");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid sample-coord reject reason");

    fuse::renderer::FroxelSampleCoords normalized = outOfRange;
    fuse::renderer::FroxelGridLayout::normalizeSampleCoords(normalized);
    expectNear(normalized.tx, 1.f, 1e-5f, "normalizeSampleCoords clamps tx high");
    expectNear(normalized.ty, 0.5f, 1e-5f, "normalizeSampleCoords preserves in-range ty");
    expectTrue(fuse::renderer::froxel_util::tryValidateSampleCoords(normalized, desc, coordReason),
               "normalized weights pass sample-coord validation");

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    fuse::f32 reasonSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, reasonSample, lookupReason),
               "trySample at index with reason succeeds on accessible grid");
    expectNear(reasonSample, 1.f, 1e-5f, "trySample with reason returns written density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index sample clears lookup reject reason");

    fuse::f32 rejectedReasonSample = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedReasonSample,
                                                                       lookupReason),
               "trySample at index with reason rejects empty storage");
    expectNear(rejectedReasonSample, 0.f, 1e-6f, "trySample with reason zeroes output on failure");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "failed index sample preserves lookup reject reason");

    fuse::f32 coordReasonSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordReasonSample,
                                                                     lookupReason),
               "trySample at coord with reason succeeds on accessible grid");
    expectNear(coordReasonSample, 1.f, 1e-5f, "trySample at coord with reason returns origin density");

    fuse::f32 bilinearReasonSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, inBounds, bilinearReasonSample,
                                                                       lookupReason),
               "trySampleDensityBilinear with reason succeeds on accessible grid");
    expectNear(bilinearReasonSample,
               fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, inBounds),
               1e-5f,
               "trySampleDensityBilinear with reason matches unguarded sample");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::f32 screenReasonSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenReasonSample, lookupReason),
               "trySampleDensityAtScreen with reason succeeds on accessible grid");
    expectNear(screenReasonSample,
               fuse::renderer::froxel_util::sampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               1e-5f,
               "trySampleDensityAtScreen with reason matches unguarded screen sample");
}

void testFroxelSampleCoordNormalizeAndPreflightGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelSampleCoords valid{};
    valid.tileX0 = 1u;
    valid.tileY0 = 0u;
    valid.tileX1 = 2u;
    valid.tileY1 = 1u;
    valid.sliceZ0 = 1u;
    valid.sliceZ1 = 2u;
    valid.tx = 0.25f;
    valid.ty = 0.5f;
    valid.tz = 0.75f;
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(valid, desc),
               "valid sample coords pass strict validation");
    expectTrue(fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(valid, desc),
               "valid sample coords remain in bounds");

    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryCanSampleAtCoords(valid, desc, coordReason),
               "tryCanSampleAtCoords succeeds on valid coords");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "valid coords report no sample-coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "none") == 0,
               "none sample-coord reject reason label");

    fuse::renderer::FroxelSampleCoords inverted = valid;
    inverted.tileX0 = 2u;
    inverted.tileX1 = 1u;
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidSampleCoords(inverted, desc),
               "inverted tile corners fail strict validation");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryCanSampleAtCoords(inverted, desc, coordReason),
               "tryCanSampleAtCoords rejects inverted corners");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::InvertedCorners,
               "inverted corners report inverted_corners reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "inverted_corners") == 0,
               "inverted_corners sample-coord reject reason label");

    fuse::renderer::FroxelGridLayout::normalizeSampleCoords(inverted);
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(inverted, desc),
               "normalizeSampleCoords repairs inverted corners");
    expectTrue(inverted.tileX0 == 1u && inverted.tileX1 == 2u, "normalizeSampleCoords preserves tile span");

    fuse::renderer::FroxelSampleCoords outOfRange = valid;
    outOfRange.tileX0 = desc.tilesX;
    outOfRange.tileX1 = desc.tilesX;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryCanSampleAtCoords(outOfRange, desc, coordReason),
               "tryCanSampleAtCoords rejects OOB tile coord");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRange,
               "OOB tile coord reports out_of_range reject reason");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryCanSampleAtCoords(valid, zeroDesc, coordReason),
               "tryCanSampleAtCoords rejects empty froxel desc");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid sample-coord reject reason");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    expectTrue(fuse::renderer::froxel_util::canSampleAtCoords(grid, desc, valid),
               "accessible grid passes coord-based sample preflight");

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, valid, lookupReason, coordReason),
               "tryCanSampleAtCoords succeeds when grid and coords are valid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "valid coord sample reports no lookup reject reason");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "valid coord sample reports no coord reject reason");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(emptyGrid, desc, valid, lookupReason, coordReason),
               "tryCanSampleAtCoords rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage lookup reject reason");

    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, outOfRange, lookupReason, coordReason),
               "tryCanSampleAtCoords rejects OOB sample coords");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRange,
               "OOB sample coords report out_of_range coord reject reason");

    fuse::renderer::VolumetricFogParams params{};
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, params),
               "default params do not skip froxel populate");
    params.density = 0.f;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, params),
               "zero density skips froxel populate");
    params.density = 0.02f;
    params.march_steps = 0u;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, params),
               "zero march steps skip froxel populate");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroDesc, params),
               "empty froxel desc skips populate");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "tryValidateGridDensityForDesc accepts accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "accessible grid reports no density reject reason");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, mismatched, densityReason),
               "tryValidateGridDensityForDesc rejects desc mismatch");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "desc mismatch reports undersized_storage reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(
                   fuse::renderer::GridDensityRejectReason::EmptyDesc),
               "empty_desc") == 0,
               "empty_desc density reject reason label");

    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc vacuously succeeds on empty desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "empty desc validation reports no density reject reason");

    grid.density[0] = 1.25f;
    fuse::f32 indexedSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, indexedSample, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectNear(indexedSample, 1.25f, 1e-5f, "trySampleDensityAtIndex with reason returns stored density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful indexed sample reports no lookup reject reason");

    fuse::f32 rejectedIndexed = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedIndexed, lookupReason),
               "trySampleDensityAtIndex with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "rejected indexed sample reports empty_storage lookup reject reason");
    expectNear(rejectedIndexed, 0.f, 1e-6f, "trySampleDensityAtIndex with reason zeroes output on rejection");
}

void testFroxelSampleCoordAndDensityPreflightGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleCoords mapped{};
    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.25f, 0.5f, 10.f, desc, camera, mapped, coordReason),
               "tryMapScreenDepthToSampleCoords succeeds in range");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "in-range mapping reports no coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "none") == 0,
               "none sample-coord reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::FroxelSampleCoords rejectedCoords{};
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, rejectedCoords, coordReason),
               "tryMapScreenDepthToSampleCoords rejects empty grid");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty grid reports empty_grid coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "empty_grid") == 0,
               "empty_grid sample-coord reject reason label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, rejectedCoords, coordReason),
               "tryMapScreenDepthToSampleCoords rejects depth below near");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthBelowNear,
               "below-near depth reports depth_below_near reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "depth_below_near") == 0,
               "depth_below_near sample-coord reject reason label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 200.f, desc, camera, rejectedCoords, coordReason),
               "tryMapScreenDepthToSampleCoords rejects depth above far");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthAboveFar,
               "above-far depth reports depth_above_far reject reason");

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
               "reversed sample coord corners fail validity check");
    fuse::renderer::FroxelGridLayout::normalizeSampleCoords(reversed);
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(reversed, desc),
               "normalizeSampleCoords fixes reversed corners");

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
    expectTrue(fuse::renderer::froxel_util::canSampleAtCoords(grid, desc, inBounds),
               "canSampleAtCoords accepts valid coords on accessible grid");

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, inBounds, lookupReason, coordReason),
               "tryCanSampleAtCoords succeeds on valid coords");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "valid coord sample reports no lookup reject reason");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "valid coord sample reports no coord reject reason");

    fuse::renderer::FroxelSampleCoords oobCoords = inBounds;
    oobCoords.tileX0 = 99u;
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, oobCoords, lookupReason, coordReason),
               "tryCanSampleAtCoords rejects OOB tile coord");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "OOB tile coord reports out_of_bounds reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "out_of_bounds") == 0,
               "out_of_bounds sample-coord reject reason label");

    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index sample reports no lookup reject reason");
    expectNear(sampled, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns stored density");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, sampled, lookupReason),
               "trySampleDensityAtIndex with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage index sample reports empty_storage reject reason");

    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 2.5f, lookupReason),
               "tryWriteDensityAtIndex with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index write reports no lookup reject reason");

    fuse::f32 coordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordSample, lookupReason),
               "trySampleDensityAtCoord with reason succeeds on accessible grid");
    expectNear(coordSample, 1.f, 1e-5f, "trySampleDensityAtCoord with reason returns stored density");

    fuse::f32 bilinearSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(
                   grid, desc, inBounds, bilinearSample, lookupReason, coordReason),
               "trySampleDensityBilinear with reason succeeds on valid coords");
    expectNear(bilinearSample,
               fuse::renderer::froxel_util::sampleDensityBilinear(grid, desc, inBounds),
               1e-5f,
               "trySampleDensityBilinear with reason matches unguarded sample");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(
                   emptyGrid, desc, inBounds, bilinearSample, lookupReason, coordReason),
               "trySampleDensityBilinear with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage bilinear sample reports empty_storage reject reason");

    fuse::f32 trilinearSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(
                   grid, desc, inBounds, trilinearSample, lookupReason, coordReason),
               "trySampleDensityTrilinear with reason succeeds on valid coords");

    fuse::f32 screenSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, lookupReason, coordReason),
               "trySampleDensityAtScreen with reason succeeds on accessible grid");
    expectNear(screenSample,
               fuse::renderer::froxel_util::sampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               1e-5f,
               "trySampleDensityAtScreen with reason matches unguarded screen sample");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, screenSample, lookupReason, coordReason),
               "trySampleDensityAtScreen with reason rejects below-near depth");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthBelowNear,
               "below-near screen sample reports depth_below_near reject reason");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "tryValidateGridDensityForDesc succeeds on accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "valid grid reports no density reject reason for desc validation");

    fuse::renderer::FroxelDensityGrid undersized{};
    undersized.density.resize(desc.froxelCount() - 1u, 0.f);
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(undersized, desc, densityReason),
               "tryValidateGridDensityForDesc rejects undersized storage");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "undersized storage reports undersized_storage reject reason for desc validation");
}

void testFroxelSampleCoordPreflightAndEmptyGridValidation() {
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
    inBounds.tx = 0.25f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.75f;

    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(inBounds, desc, coordReason),
               "tryAreSampleCoordsInBounds accepts in-bounds coords");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "in-bounds sample coords report no reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "none") == 0,
               "none sample-coord reject reason label");

    fuse::renderer::FroxelSampleCoords badTile = inBounds;
    badTile.tileX0 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(badTile, desc, coordReason),
               "tryAreSampleCoordsInBounds rejects OOB tile");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRangeTile,
               "OOB tile reports out_of_range_tile reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "out_of_range_tile") == 0,
               "out_of_range_tile sample-coord reject reason label");

    fuse::renderer::FroxelSampleCoords badWeight = inBounds;
    badWeight.tx = 2.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(badWeight, desc, coordReason),
               "tryAreSampleCoordsInBounds rejects OOB interpolation weight");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::OutOfRangeWeight,
               "OOB weight reports out_of_range_weight reject reason");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryAreSampleCoordsInBounds(inBounds, zeroDesc, coordReason),
               "tryAreSampleCoordsInBounds rejects empty froxel grid");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty froxel grid reports empty_grid sample-coord reject reason");

    fuse::renderer::FroxelSampleCoords reversed = inBounds;
    reversed.tileX0 = 2u;
    reversed.tileX1 = 1u;
    reversed.tx = 0.75f;
    fuse::renderer::FroxelGridLayout::normalizeSampleCoords(reversed);
    expectTrue(fuse::renderer::FroxelGridLayout::areSampleCoordsInBounds(reversed, desc),
               "normalizeSampleCoords fixes reversed tile corners");
    expectTrue(reversed.tileX0 == 1u && reversed.tileX1 == 2u, "normalizeSampleCoords preserves tile ordering");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleCoords mapped{};
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.25f, 0.25f, 3.16f, desc, camera, mapped, coordReason),
               "tryMapScreenDepthToSampleCoords succeeds on valid input");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "valid screen mapping reports no sample-coord reject reason");

    fuse::u32 froxelIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.25f, 0.25f, 3.16f, desc, camera, froxelIndex, coordReason),
               "tryMapScreenDepthToFroxelIndex succeeds on valid input");
    expectTrue(froxelIndex < desc.froxelCount(), "tryMapScreenDepthToFroxelIndex yields in-bounds index");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.5f, desc, camera, mapped, coordReason),
               "tryMapScreenDepthToSampleCoords rejects depth below near plane");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "depth_out_of_range") == 0,
               "depth_out_of_range sample-coord reject reason label");

    fuse::renderer::FroxelCameraDesc badCamera = camera;
    badCamera.nearPlane = 0.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, desc, badCamera, froxelIndex, coordReason),
               "tryMapScreenDepthToFroxelIndex rejects invalid camera");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera reject reason");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "tryValidateGridDensityForDesc accepts accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "accessible grid reports no density reject reason");

    fuse::renderer::FroxelDensityGrid emptyGridForValidation{};
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGridForValidation, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc vacuously succeeds when both grid and desc are empty");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "empty grid and empty desc report no density reject reason");
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc rejects non-empty storage on empty desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::EmptyDesc,
               "non-empty storage on empty desc reports empty_desc reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "empty_desc") == 0,
               "empty_desc density reject reason label");

    expectTrue(fuse::renderer::froxel_util::writeDensityAtIndex(grid, desc, 0u, 1.f),
               "seed origin density for lookup preflight tests");

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index lookup reports no reject reason");
    expectNear(sampled, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns density");

    fuse::renderer::FroxelSampleCoords oobCoords = inBounds;
    oobCoords.tileX0 = 99u;
    fuse::f32 rejectedBilinear = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, oobCoords, rejectedBilinear,
                                                                      lookupReason),
               "trySampleDensityBilinear with reason rejects OOB sample coords");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::SampleCoordRejected,
               "OOB bilinear sample reports sample_coord_rejected lookup reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "sample_coord_rejected") == 0,
               "sample_coord_rejected lookup reject reason label");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.5f, rejectedScreen, lookupReason),
               "trySampleDensityAtScreen with reason rejects depth below near plane");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::ScreenMappingFailed,
               "failed screen mapping reports screen_mapping_failed lookup reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "screen_mapping_failed") == 0,
               "screen_mapping_failed lookup reject reason label");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(emptyGrid, desc, inBounds, rejectedBilinear,
                                                                       lookupReason),
               "trySampleDensityTrilinear with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage trilinear lookup reports empty_storage reject reason");
}

void testFroxelSampleCoordMappingPreflightGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleCoords coords{};
    fuse::renderer::SampleCoordRejectReason reason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.25f, 0.5f, 10.f, desc, camera, coords, reason),
               "tryMapScreenDepthToSampleCoords succeeds in range");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::None,
               "successful mapping reports no sample reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "none") == 0,
               "none sample reject reason label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, coords, reason),
               "tryMapScreenDepthToSampleCoords rejects depth below near");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::DepthBelowNear,
               "below-near depth reports depth_below_near reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "depth_below_near") == 0,
               "depth_below_near sample reject reason label");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 200.f, desc, camera, coords, reason),
               "tryMapScreenDepthToSampleCoords rejects depth above far");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::DepthAboveFar,
               "above-far depth reports depth_above_far reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "depth_above_far") == 0,
               "depth_above_far sample reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, coords, reason),
               "tryMapScreenDepthToSampleCoords rejects empty grid");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid sample reject reason label");

    fuse::u32 froxelIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, desc, camera, froxelIndex, reason),
               "tryMapScreenDepthToFroxelIndex succeeds in range");
    expectTrue(froxelIndex < desc.froxelCount(), "tryMap froxel index in bounds");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::None,
               "successful froxel index mapping reports no reject reason");

    fuse::u32 rejectedIndex = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 0.01f, desc, camera, rejectedIndex, reason),
               "tryMapScreenDepthToFroxelIndex rejects below-near depth");
    expectTrue(rejectedIndex == 0u, "tryMapScreenDepthToFroxelIndex zeroes output on rejection");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::DepthBelowNear,
               "froxel index mapping preserves below-near reject reason");
}

void testFroxelSampleCoordBoundsPreflightGuards() {
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
    inBounds.tx = 0.25f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.75f;

    fuse::renderer::SampleCoordBoundsRejectReason boundsReason =
        fuse::renderer::SampleCoordBoundsRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateSampleCoords(inBounds, desc, boundsReason),
               "tryValidateSampleCoords accepts in-bounds coords");
    expectTrue(boundsReason == fuse::renderer::SampleCoordBoundsRejectReason::None,
               "in-bounds coords report no bounds reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordBoundsRejectReasonLabel(boundsReason), "none") == 0,
               "none bounds reject reason label");

    fuse::renderer::FroxelSampleCoords oobTile = inBounds;
    oobTile.tileX0 = 99u;
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(oobTile, desc, boundsReason),
               "tryValidateSampleCoords rejects OOB tile coord");
    expectTrue(boundsReason == fuse::renderer::SampleCoordBoundsRejectReason::OutOfBoundsTile,
               "OOB tile reports out_of_bounds_tile reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordBoundsRejectReasonLabel(boundsReason), "out_of_bounds_tile") ==
                   0,
               "out_of_bounds_tile bounds reject reason label");

    fuse::renderer::FroxelSampleCoords oobWeight = inBounds;
    oobWeight.tx = 2.f;
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(oobWeight, desc, boundsReason),
               "tryValidateSampleCoords rejects OOB interpolation weight");
    expectTrue(boundsReason == fuse::renderer::SampleCoordBoundsRejectReason::OutOfBoundsWeight,
               "OOB weight reports out_of_bounds_weight reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordBoundsRejectReasonLabel(boundsReason),
                           "out_of_bounds_weight") == 0,
               "out_of_bounds_weight bounds reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::froxel_util::tryValidateSampleCoords(inBounds, zeroDesc, boundsReason),
               "tryValidateSampleCoords rejects empty grid desc");
    expectTrue(boundsReason == fuse::renderer::SampleCoordBoundsRejectReason::EmptyGrid,
               "empty grid reports empty_grid bounds reject reason");
}

void testFroxelDensityLookupAndValidationPreflightGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    expectTrue(fuse::renderer::froxel_util::writeDensityAtIndex(grid, desc, 0u, 1.25f),
               "seed origin density for lookup reason tests");

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectNear(sampled, 1.25f, 1e-5f, "trySampleDensityAtIndex with reason returns written density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful sample clears lookup reject reason");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    fuse::f32 rejectedSample = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedSample, lookupReason),
               "trySampleDensityAtIndex with reason rejects empty storage");
    expectNear(rejectedSample, 0.f, 1e-6f, "trySampleDensityAtIndex with reason zeroes output on failure");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "failed sample preserves empty_storage lookup reject reason");

    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(emptyGrid, desc, 0u, 9.f, lookupReason),
               "tryWriteDensityAtIndex with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "failed write preserves empty_storage lookup reject reason");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, mismatched, 0u, 9.f, lookupReason),
               "tryWriteDensityAtIndex with reason rejects desc mismatch");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::DescMismatch,
               "failed write reports desc_mismatch lookup reject reason");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "tryValidateGridDensityForDesc accepts accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "accessible grid reports no density reject reason");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc vacuously succeeds on empty desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "empty desc reports no density reject reason");

    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, mismatched, densityReason),
               "tryValidateGridDensityForDesc rejects desc mismatch");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch density reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "desc_mismatch") == 0,
               "desc_mismatch density reject reason label");

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

    fuse::f32 bilinearSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityBilinear(grid, desc, coords, bilinearSample, lookupReason),
               "trySampleDensityBilinear with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful bilinear sample clears lookup reject reason");

    fuse::f32 rejectedBilinear = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityBilinear(
                   emptyGrid, desc, coords, rejectedBilinear, lookupReason),
               "trySampleDensityBilinear with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "failed bilinear sample preserves lookup reject reason");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::f32 screenSample = 0.f;
    fuse::renderer::SampleCoordRejectReason sampleReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, lookupReason, sampleReason),
               "trySampleDensityAtScreen with reasons succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful screen sample clears lookup reject reason");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::None,
               "successful screen sample clears sample reject reason");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, rejectedScreen, lookupReason, sampleReason),
               "trySampleDensityAtScreen with reasons rejects below-near depth");
    expectNear(rejectedScreen, 0.f, 1e-6f, "trySampleDensityAtScreen zeroes output on mapping failure");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::DepthBelowNear,
               "below-near screen sample reports depth_below_near reason");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "mapping failure does not set lookup reject reason");
}

void testFroxelSampleCoordPreflightAndNormalizeGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleCoords mapped{};
    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::tryMapScreenDepthToSampleCoords(0.25f, 0.5f, 10.f, desc, camera, mapped, coordReason),
               "tryMap succeeds on valid screen depth");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "valid mapping reports no sample-coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "none") == 0,
               "none sample-coord reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::FroxelSampleCoords rejectedCoords{};
    expectTrue(!fuse::renderer::tryMapScreenDepthToSampleCoords(0.5f, 0.5f, 10.f, zeroDesc, camera, rejectedCoords,
                                                                coordReason),
               "tryMap rejects empty froxel grid");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample-coord reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "empty_grid") == 0,
               "empty_grid sample-coord reject reason label");

    expectTrue(!fuse::renderer::tryMapScreenDepthToSampleCoords(0.5f, 0.5f, 0.01f, desc, camera, rejectedCoords,
                                                                coordReason),
               "tryMap rejects depth below near plane");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "depth_out_of_range") == 0,
               "depth_out_of_range sample-coord reject reason label");

    expectTrue(!fuse::renderer::tryMapScreenDepthToSampleCoords(0.5f, 0.5f, 200.f, desc, camera, rejectedCoords,
                                                                coordReason),
               "tryMap rejects depth above far plane");

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
    expectTrue(reversed.tileX0 > reversed.tileX1 && reversed.tileY0 > reversed.tileY1 &&
                   reversed.sliceZ0 > reversed.sliceZ1,
               "reversed corners are out of canonical order before normalize");
    fuse::renderer::FroxelGridLayout::normalizeSampleCoords(reversed);
    expectTrue(reversed.tileX0 <= reversed.tileX1 && reversed.tileY0 <= reversed.tileY1 &&
                   reversed.sliceZ0 <= reversed.sliceZ1,
               "normalize fixes reversed tile/slice corners");
    expectTrue(reversed.tx >= 0.f && reversed.tx <= 1.f && reversed.ty >= 0.f && reversed.ty <= 1.f &&
                   reversed.tz >= 0.f && reversed.tz <= 1.f,
               "normalize clamps interpolation weights");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;

    fuse::f32 lookupSample = 0.f;
    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, lookupSample, lookupReason),
               "trySample at index with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index lookup reports no reject reason");
    expectNear(lookupSample, 1.f, 1e-5f, "trySample at index with reason returns density");

    fuse::f32 rejectedLookup = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, zeroDesc, 0u, rejectedLookup, lookupReason),
               "trySample at index with reason rejects empty froxel desc");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid lookup reject reason");

    fuse::renderer::FroxelSampleCoords inBounds = mapped;
    fuse::f32 trilinearSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinear(grid, desc, inBounds, trilinearSample,
                                                                      lookupReason),
               "trySample trilinear with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful trilinear lookup reports no reject reason");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    fuse::f32 rejectedTrilinear = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinear(emptyGrid, desc, inBounds, rejectedTrilinear,
                                                                       lookupReason),
               "trySample trilinear with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage lookup reject reason");

    fuse::f32 screenSample = 0.f;
    fuse::renderer::SampleCoordRejectReason screenCoordReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f,
                                                                     screenSample, lookupReason, screenCoordReason),
               "trySample at screen with reasons succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful screen sample reports no lookup reject reason");
    expectTrue(screenCoordReason == fuse::renderer::SampleCoordRejectReason::None,
               "successful screen sample reports no coord reject reason");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(emptyGrid, desc, camera, 0.5f, 0.5f, 10.f,
                                                                      rejectedScreen, lookupReason, screenCoordReason),
               "trySample at screen with reasons rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "rejected screen sample reports empty_storage lookup reason");
    expectTrue(screenCoordReason == fuse::renderer::SampleCoordRejectReason::None,
               "lookup rejection short-circuits before coord mapping");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(grid, desc, camera, 0.5f, 0.5f, 0.01f,
                                                                      rejectedScreen, lookupReason, screenCoordReason),
               "trySample at screen with reasons rejects out-of-range depth");
    expectTrue(screenCoordReason == fuse::renderer::SampleCoordRejectReason::DepthOutOfRange,
               "out-of-range depth reports depth_out_of_range coord reason");
}

void testFroxelGridDensityValidationForDescGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(fuse::renderer::froxel_util::canAllocateFroxelGrid(desc),
               "non-empty froxel desc can allocate");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc),
               "non-empty froxel desc does not skip populate");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::froxel_util::canAllocateFroxelGrid(zeroDesc),
               "zero-dimension desc cannot allocate");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroDesc),
               "zero-dimension desc skips populate");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "allocated grid validates for matching desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "valid grid reports no density reject reason");

    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, zeroDesc, densityReason),
               "empty desc vacuously validates density for desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "empty desc reports no density reject reason");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, mismatched, densityReason),
               "desc mismatch fails density validation for desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch density reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "desc_mismatch") == 0,
               "desc_mismatch density reject reason label");

    fuse::renderer::FroxelDensityGrid undersized{};
    undersized.density.resize(desc.froxelCount() - 1u, 0.f);
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(undersized, desc, densityReason),
               "undersized storage fails density validation for desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "undersized storage reports undersized_storage reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "undersized_storage") == 0,
               "undersized_storage density reject reason label");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, desc, densityReason),
               "empty storage fails density validation for desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "empty storage on non-empty desc reports undersized_storage");
}

void testFroxelPreflightGuardsDeepen() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

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
    expectTrue(fuse::renderer::froxel_util::canSampleValidCoords(grid, desc, inBounds),
               "canSampleValidCoords accepts in-bounds coords");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelSample(grid, desc, inBounds),
               "wouldSkipFroxelSample does not skip accessible in-bounds sample");

    fuse::renderer::FroxelSampleCoords invalidWeights = inBounds;
    invalidWeights.tx = 2.f;
    expectTrue(!fuse::renderer::froxel_util::canSampleValidCoords(grid, desc, invalidWeights),
               "canSampleValidCoords rejects OOB interpolation weights");
    expectTrue(!fuse::renderer::froxel_util::wouldSkipFroxelSample(grid, desc, invalidWeights),
               "wouldSkipFroxelSample still proceeds when weights will be clamped");
    expectTrue(fuse::renderer::froxel_util::classifyFroxelSampleReject(grid, desc, invalidWeights) ==
                   fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "classifyFroxelSampleReject reports invalid_weights for OOB t");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    fuse::renderer::SampleCoordRejectReason skipReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::wouldSkipFroxelSample(emptyGrid, desc, inBounds, &skipReason),
               "wouldSkipFroxelSample skips empty storage");
    expectTrue(skipReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "empty storage maps to out_of_bounds sample skip reason");

    fuse::f32 sampled = 0.f;
    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, sampled, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectNear(sampled, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index sample reports no lookup reject reason");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, sampled, lookupReason),
               "trySampleDensityAtIndex with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage lookup reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "empty_storage") == 0,
               "empty_storage lookup reject reason label");

    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 2.5f, lookupReason),
               "tryWriteDensityAtIndex with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index write reports no lookup reject reason");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(emptyGrid, desc, 0u, 1.f, lookupReason),
               "tryWriteDensityAtIndex with reason rejects empty storage");

    fuse::f32 coordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordSample, lookupReason),
               "trySampleDensityAtCoord with reason succeeds on accessible grid");
    expectNear(coordSample, 1.f, 1e-5f, "trySampleDensityAtCoord with reason returns density");
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 3.25f, lookupReason),
               "tryWriteDensityAtCoord with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful coord write reports no lookup reject reason");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "tryValidateGridDensityForDesc accepts accessible grid");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "accessible grid reports no density reject reason");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, zeroDesc, densityReason),
               "tryValidateGridDensityForDesc vacuously succeeds on empty desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "empty desc validation reports no reject reason");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::f32 screenSample = 0.f;
    fuse::renderer::ScreenMappingRejectReason mapReason = fuse::renderer::ScreenMappingRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, mapReason),
               "trySampleDensityAtScreen with reason succeeds in range");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::None,
               "successful screen sample reports no mapping reject reason");
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, screenSample, mapReason),
               "trySampleDensityAtScreen with reason rejects below-near depth");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range mapping reject reason");

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.march_steps = 32u;
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, params),
               "non-empty desc with density does not skip populate");

    fuse::renderer::FroxelPopulateRejectReason populateReason = fuse::renderer::FroxelPopulateRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, params, populateReason),
               "tryCanPopulateFromAnalyticFog accepts enabled params");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::None,
               "enabled populate reports no reject reason");

    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, zeroDensity),
               "zero density skips froxel populate");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, zeroDensity, populateReason),
               "tryCanPopulateFromAnalyticFog rejects zero density");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroDensity,
               "zero density reports zero_density populate reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "zero_density") == 0,
               "zero_density populate reject reason label");

    fuse::renderer::VolumetricFogParams zeroMarch{};
    zeroMarch.density = 0.02f;
    zeroMarch.march_steps = 0u;
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, zeroMarch, populateReason),
               "tryCanPopulateFromAnalyticFog rejects zero march steps");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroMarchSteps,
               "zero march steps reports zero_march_steps populate reject reason");

    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(zeroDesc, params, populateReason),
               "tryCanPopulateFromAnalyticFog rejects empty froxel desc");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid populate reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "empty_grid") == 0,
               "empty_grid populate reject reason label");
}

void testFroxelPreflightValidateAndLookupReasonGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

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
    expectTrue(fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(inBounds, desc, sampleReason),
               "tryValidateSampleCoords accepts in-bounds coords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::None,
               "valid sample coords report no reject reason");

    fuse::renderer::FroxelSampleCoords reversed = inBounds;
    reversed.tileX0 = 2u;
    reversed.tileX1 = 1u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(reversed, desc, sampleReason),
               "tryValidateSampleCoords rejects unordered corners");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::UnorderedCorners,
               "unordered corners report unordered_corners reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(sampleReason), "unordered_corners") == 0,
               "unordered_corners sample reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::FroxelSampleCoords anyCoords{};
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(anyCoords, zeroDesc, sampleReason),
               "tryValidateSampleCoords rejects empty grid");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample reject reason");

    fuse::renderer::GridDensityRejectReason gridReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryPreflightNonEmptyGrid(desc, gridReason),
               "tryPreflightNonEmptyGrid succeeds on non-empty grid");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::None,
               "non-empty grid reports no density reject reason");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryPreflightNonEmptyGrid(zeroDesc, gridReason),
               "tryPreflightNonEmptyGrid rejects empty grid");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::EmptyDesc,
               "empty grid reports empty_desc density reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(gridReason), "empty_desc") == 0,
               "empty_desc grid density reject reason label");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    expectTrue(fuse::renderer::froxel_util::tryPreflightDensityGridAccess(grid, desc, gridReason),
               "tryPreflightDensityGridAccess succeeds on accessible grid");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::None,
               "accessible grid reports no preflight reject reason");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryPreflightDensityGridAccess(emptyGrid, desc, gridReason),
               "tryPreflightDensityGridAccess rejects empty storage");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::UndersizedStorage,
               "empty storage reports undersized_storage preflight reject reason");

    expectTrue(!fuse::renderer::froxel_util::tryPreflightDensityGridAccess(grid, zeroDesc, gridReason),
               "tryPreflightDensityGridAccess rejects empty desc");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::EmptyDesc,
               "empty desc reports empty_desc preflight reject reason");

    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, gridReason),
               "tryValidateGridDensityForDesc accepts accessible grid");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::None,
               "accessible grid validates for desc with no reject reason");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, mismatched, gridReason),
               "tryValidateGridDensityForDesc rejects desc mismatch");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch validation reject reason");

    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(emptyGrid, zeroDesc, gridReason),
               "tryValidateGridDensityForDesc vacuously succeeds on empty desc");
    expectTrue(gridReason == fuse::renderer::GridDensityRejectReason::None,
               "empty desc validation reports no reject reason");

    expectTrue(fuse::renderer::froxel_util::writeDensityAtIndex(grid, desc, 3u, 4.25f),
               "seed density for lookup reason tests");

    fuse::f32 sampled = 0.f;
    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 3u, sampled, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectNear(sampled, 4.25f, 1e-5f, "trySampleDensityAtIndex with reason returns written density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index sample reports no lookup reject reason");

    fuse::f32 rejectedSample = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedSample, lookupReason),
               "trySampleDensityAtIndex with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage lookup reject reason");

    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 5.5f, lookupReason),
               "tryWriteDensityAtCoord with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful coord write reports no lookup reject reason");

    fuse::f32 coordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 1u, 1u, 2u, coordSample, lookupReason),
               "trySampleDensityAtCoord with reason succeeds on accessible grid");
    expectNear(coordSample, 5.5f, 1e-5f, "trySampleDensityAtCoord with reason returns written density");

    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, mismatched, 0u, 0u, 0u, 9.f, lookupReason),
               "tryWriteDensityAtCoord with reason rejects desc mismatch");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch lookup reject reason");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::f32 screenSample = 0.f;
    fuse::renderer::ScreenMappingRejectReason mapReason = fuse::renderer::ScreenMappingRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, mapReason),
               "trySampleDensityAtScreen with reason succeeds in range");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::None,
               "successful screen sample reports no mapping reject reason");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   emptyGrid, desc, camera, 0.5f, 0.5f, 10.f, rejectedScreen, mapReason),
               "trySampleDensityAtScreen with reason rejects empty storage");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::EmptyGrid,
               "empty storage maps to empty_grid screen reject reason");

    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, rejectedScreen, mapReason),
               "trySampleDensityAtScreen with reason rejects depth below near plane");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range screen reject reason");
}

void testFroxelPopulatePreflightAndIndexValidationGuards() {
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

    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, params),
               "enabled fog params do not skip populate");
    expectTrue(fuse::renderer::froxel_util::canPopulateFromAnalyticFog(desc, camera, params),
               "valid desc/camera/params pass populate preflight");

    fuse::renderer::FroxelPopulateRejectReason populateReason = fuse::renderer::FroxelPopulateRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, params, populateReason),
               "tryCanPopulate succeeds on valid inputs");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::None,
               "valid populate reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "none") == 0,
               "none populate reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroDesc, params),
               "empty froxel desc skips populate");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(zeroDesc, camera, params, populateReason),
               "empty froxel desc fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid populate reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "empty_grid") == 0,
               "empty_grid populate reject reason label");

    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, zeroDensity),
               "zero density skips populate");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroDensity, populateReason),
               "zero density fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroDensity,
               "zero density reports zero_density populate reject reason");

    fuse::renderer::VolumetricFogParams zeroMarch{};
    zeroMarch.density = 0.02f;
    zeroMarch.march_steps = 0u;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, zeroMarch),
               "zero march steps skip populate");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroMarch, populateReason),
               "zero march steps fail populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroMarchSteps,
               "zero march steps report zero_march_steps populate reject reason");

    fuse::renderer::FroxelCameraDesc badCamera{};
    badCamera.nearPlane = 100.f;
    badCamera.farPlane = 1.f;
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, badCamera, params, populateReason),
               "invalid camera fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera populate reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "invalid_camera") == 0,
               "invalid_camera populate reject reason label");

    fuse::renderer::FroxelDensityGrid grid{};
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(grid, desc, camera, params, populateReason),
               "tryPopulate succeeds on valid inputs");
    expectTrue(grid.matchesDesc(desc), "tryPopulate allocates matching density storage");
    expectTrue(fuse::renderer::froxel_util::countNonZeroFroxels(grid) == desc.froxelCount(),
               "tryPopulate fills all froxels with non-zero density");

    fuse::renderer::FroxelDensityGrid rejectedGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(
                   rejectedGrid, zeroDesc, camera, params, populateReason),
               "tryPopulate rejects empty froxel desc");
    expectTrue(rejectedGrid.isEmpty(), "rejected populate leaves storage empty");

    fuse::renderer::DensityLookupRejectReason indexReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateFroxelIndex(5u, desc, indexReason),
               "in-range froxel index passes validation");
    expectTrue(indexReason == fuse::renderer::DensityLookupRejectReason::None,
               "in-range index reports no reject reason");
    expectTrue(!fuse::renderer::froxel_util::tryValidateFroxelIndex(99u, desc, indexReason),
               "OOB froxel index fails validation");
    expectTrue(indexReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "OOB index reports index_out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(indexReason), "index_out_of_range") == 0,
               "index_out_of_range lookup reject reason label");
    expectTrue(!fuse::renderer::froxel_util::tryValidateFroxelIndex(0u, zeroDesc, indexReason),
               "empty grid fails froxel index validation");
    expectTrue(indexReason == fuse::renderer::DensityLookupRejectReason::EmptyGrid,
               "empty grid reports empty_grid index reject reason");

    fuse::f32 indexedSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, indexedSample, indexReason),
               "trySample at index with reason succeeds on accessible grid");
    expectTrue(indexReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful indexed sample reports no lookup reject reason");
    expectTrue(indexedSample > 0.f, "indexed sample returns populated density");

    fuse::renderer::FroxelSampleCoords validCoords{};
    validCoords.tileX0 = 0u;
    validCoords.tileY0 = 0u;
    validCoords.tileX1 = 1u;
    validCoords.tileY1 = 1u;
    validCoords.sliceZ0 = 0u;
    validCoords.sliceZ1 = 1u;
    validCoords.tx = 0.5f;
    validCoords.ty = 0.5f;
    validCoords.tz = 0.5f;

    fuse::renderer::SampleCoordRejectReason sampleReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(validCoords, desc, sampleReason),
               "valid sample coords pass tryValidateSampleCoords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::None,
               "valid sample coords report no reject reason");

    fuse::renderer::FroxelSampleCoords reversed = validCoords;
    reversed.tileX0 = 2u;
    reversed.tileX1 = 1u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(reversed, desc, sampleReason),
               "reversed sample corners fail tryValidateSampleCoords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::UnorderedCorners,
               "reversed corners report unordered_corners reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(sampleReason), "unordered_corners") == 0,
               "unordered_corners sample reject reason label");

    fuse::renderer::FroxelSampleCoords oobIndices = validCoords;
    oobIndices.tileX0 = 99u;
    oobIndices.tileX1 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(oobIndices, desc, sampleReason),
               "OOB tile indices fail tryValidateSampleCoords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "OOB tile indices report out_of_bounds reject reason");

    fuse::renderer::FroxelSampleCoords oobWeights = validCoords;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(oobWeights, desc, sampleReason),
               "OOB interpolation weights fail tryValidateSampleCoords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "OOB weights report invalid_weights reject reason");

    fuse::f32 screenSample = 0.f;
    fuse::renderer::ScreenMappingRejectReason screenReason = fuse::renderer::ScreenMappingRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, screenReason),
               "trySampleDensityAtScreen with reason succeeds in range");
    expectTrue(screenReason == fuse::renderer::ScreenMappingRejectReason::None,
               "successful screen sample reports no mapping reject reason");
    expectTrue(screenSample > 0.f, "screen sample returns populated density");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, rejectedScreen, screenReason),
               "trySampleDensityAtScreen with reason rejects below-near depth");
    expectTrue(screenReason == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range screen reject reason");
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
    params.march_steps = 16u;

    expectTrue(fuse::renderer::froxel_util::isValidPopulateCamera(camera),
               "default camera passes populate camera validation");
    expectTrue(fuse::renderer::froxel_util::canPopulateFromAnalyticFog(desc, camera, params),
               "valid desc/camera/params can populate non-zero density");
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, camera, params),
               "valid populate inputs do not skip");

    fuse::renderer::FroxelPopulateRejectReason populateReason = fuse::renderer::FroxelPopulateRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(desc, camera, params, populateReason),
               "preflight populate succeeds for valid inputs");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::None,
               "valid populate reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "none") == 0,
               "none populate reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::froxel_util::canPopulateFromAnalyticFog(zeroDesc, camera, params),
               "empty froxel desc cannot populate non-zero density");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroDesc, camera, params),
               "empty froxel desc skips populate");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(zeroDesc, camera, params, populateReason),
               "empty froxel desc fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid populate reject reason");
    expectTrue(std::strcmp(fuse::renderer::froxelPopulateRejectReasonLabel(populateReason), "empty_grid") == 0,
               "empty_grid populate reject reason label");

    fuse::renderer::FroxelDensityGrid rejectedGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(rejectedGrid, zeroDesc, camera, params),
               "tryPopulate rejects empty froxel desc without mutating storage");
    expectTrue(rejectedGrid.isEmpty(), "rejected populate leaves storage empty");

    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    expectTrue(!fuse::renderer::froxel_util::canPopulateFromAnalyticFog(desc, camera, zeroDensity),
               "zero density cannot populate non-zero froxels");
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroDensity, populateReason),
               "zero density populate preflight still succeeds for allocation path");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroDensity,
               "zero density reports zero_density populate reject reason");

    fuse::renderer::VolumetricFogParams zeroMarch{};
    zeroMarch.density = 0.02f;
    zeroMarch.march_steps = 0u;
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroMarch, populateReason),
               "zero march populate preflight still succeeds for allocation path");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::ZeroMarchSteps,
               "zero march reports zero_march_steps populate reject reason");

    fuse::renderer::FroxelCameraDesc badCamera{};
    badCamera.nearPlane = 100.f;
    badCamera.farPlane = 1.f;
    expectTrue(!fuse::renderer::froxel_util::isValidPopulateCamera(badCamera),
               "inverted near/far fails populate camera validation");
    expectTrue(!fuse::renderer::froxel_util::preflightPopulateFromAnalyticFog(desc, badCamera, params, populateReason),
               "invalid camera fails populate preflight");
    expectTrue(populateReason == fuse::renderer::FroxelPopulateRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera populate reject reason");

    fuse::renderer::FroxelDensityGrid guardedGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(guardedGrid, desc, badCamera, params),
               "tryPopulate rejects invalid camera without filling density");
    expectTrue(guardedGrid.isEmpty(), "invalid-camera populate leaves storage empty");

    fuse::renderer::FroxelDensityGrid populated{};
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, desc, camera, params),
               "tryPopulate succeeds on valid inputs");
    expectTrue(populated.matchesDesc(desc), "guarded populate matches desc");
    expectTrue(fuse::renderer::froxel_util::hasNonZeroDensity(populated),
               "guarded populate writes non-zero density");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 999u, lookupReason),
               "OOB index lookup preflight still succeeds when storage matches desc");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "OOB index reports index_out_of_range lookup reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "index_out_of_range") == 0,
               "index_out_of_range lookup reject reason label");
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 0u, lookupReason),
               "in-bounds index lookup preflight succeeds");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "in-bounds index reports no lookup reject reason");

    fuse::renderer::FroxelSampleCoords readyCoords{};
    readyCoords.tileX0 = 0u;
    readyCoords.tileY0 = 0u;
    readyCoords.tileX1 = 1u;
    readyCoords.tileY1 = 1u;
    readyCoords.sliceZ0 = 0u;
    readyCoords.sliceZ1 = 1u;
    readyCoords.tx = 0.5f;
    readyCoords.ty = 0.5f;
    readyCoords.tz = 0.5f;
    expectTrue(fuse::renderer::froxel_util::areSampleCoordsReady(readyCoords, desc),
               "in-bounds sample coords are ready");

    fuse::renderer::FroxelSampleCoords badWeights = readyCoords;
    badWeights.tx = 2.f;
    expectTrue(!fuse::renderer::froxel_util::areSampleCoordsReady(badWeights, desc),
               "OOB interpolation weights are not sample-ready");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateGridDensityForDesc(grid, desc, densityReason),
               "accessible grid validates density for desc");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::None,
               "valid grid density-for-desc reports no reject reason");
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

void testFroxelDensityLookupRejectReasons() {
void testFroxelDeepenGuardPreflights() {
void testFroxelPopulateLookupAndStrictSampleGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[5] = 1.25f;

    fuse::renderer::DensityLookupRejectReason reason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, desc, 0u, reason),
               "tryCanLookup accepts accessible grid");
    expectTrue(reason == fuse::renderer::DensityLookupRejectReason::None, "accessible grid reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(reason), "none") == 0,
               "lookup reject label for none");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, zeroDesc, 0u, reason),
               "tryCanLookup rejects empty grid desc");
    expectTrue(reason == fuse::renderer::DensityLookupRejectReason::EmptyGrid,
               "empty grid reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(reason), "empty_grid") == 0,
               "lookup reject label for empty grid");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(emptyGrid, desc, 0u, reason),
               "tryCanLookup rejects empty storage");
    expectTrue(reason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage reason");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndex(grid, mismatched, 0u, reason),
               "tryCanLookup rejects desc mismatch");
    expectTrue(reason == fuse::renderer::DensityLookupRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(reason), "desc_mismatch") == 0,
               "lookup reject label for desc mismatch");

    fuse::f32 sampled = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 5u, sampled, reason),
               "trySample with reason succeeds on accessible grid");
    expectNear(sampled, 1.25f, 1e-5f, "trySample with reason returns stored density");
    expectTrue(reason == fuse::renderer::DensityLookupRejectReason::None,
               "successful sample clears reject reason");

    fuse::f32 rejectedSample = 9.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedSample, reason),
               "trySample with reason rejects empty storage");
    expectNear(rejectedSample, 0.f, 1e-6f, "trySample with reason zeroes output on failure");
               "failed sample preserves reject reason");

    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, mismatched, 0u, 9.f, reason),
               "tryWrite with reason rejects desc mismatch");
               "failed write preserves reject reason");

    fuse::f32 coordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, desc, 1u, 1u, 2u, 3.5f, reason),
               "tryWrite at coord with reason succeeds on accessible grid");
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 1u, 1u, 2u, coordSample, reason),
               "trySample at coord with reason succeeds on accessible grid");
    expectNear(coordSample, 3.5f, 1e-5f, "trySample at coord with reason returns written density");
               "successful coord sample clears reject reason");
}

void testFroxelSampleCoordGuards() {
    fuse::renderer::FroxelGridDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::FroxelSampleCoords valid{};
    valid.tileX0 = 1u;
    valid.tileY0 = 0u;
    valid.tileX1 = 2u;
    valid.tileY1 = 1u;
    valid.sliceZ0 = 1u;
    valid.sliceZ1 = 2u;
    valid.tx = 0.25f;
    valid.ty = 0.5f;
    valid.tz = 0.75f;
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(valid, desc),
               "in-bounds sample coords are valid");

    fuse::renderer::FroxelSampleCoords invalid{};
    invalid.tileX0 = 99u;
    invalid.tileY0 = 0u;
    invalid.tileX1 = 2u;
    invalid.tileY1 = 1u;
    invalid.sliceZ0 = 1u;
    invalid.sliceZ1 = 2u;
    invalid.tx = 0.25f;
    invalid.ty = 0.5f;
    invalid.tz = 0.75f;
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidSampleCoords(invalid, desc),
               "OOB tile corner fails sample coord validation");

    fuse::renderer::FroxelSampleCoords badWeights = valid;
    badWeights.tx = 1.5f;
    expectTrue(!fuse::renderer::FroxelGridLayout::isValidSampleCoords(badWeights, desc),
               "OOB interpolation weight fails sample coord validation");

    expectTrue(!fuse::renderer::FroxelGridLayout::isValidSampleCoords(valid, zeroDesc),
               "sample coords invalid on empty grid");

    fuse::renderer::FroxelSampleCoords toClamp = invalid;
    expectTrue(fuse::renderer::FroxelGridLayout::tryClampSampleCoords(toClamp, desc),
               "tryClamp succeeds on non-empty grid");
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(toClamp, desc),
               "tryClamp produces valid sample coords");
    expectTrue(!fuse::renderer::FroxelGridLayout::tryClampSampleCoords(toClamp, zeroDesc),
               "tryClamp rejects empty grid");

    fuse::renderer::FroxelCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::renderer::FroxelSampleCoords mapped{};
    fuse::renderer::SampleCoordRejectReason reason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 10.f, desc, camera, mapped, reason),
               "tryMap accepts in-range screen depth");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::None, "successful map reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "none") == 0,
               "sample coord reject label for none");
    expectTrue(fuse::renderer::FroxelGridLayout::isValidSampleCoords(mapped, desc),
               "mapped sample coords are valid");

    expectTrue(!fuse::renderer::FroxelGridLayout::tryMapScreenDepthToSampleCoords(
                   0.5f, 0.5f, 0.01f, desc, camera, mapped, reason),
               "tryMap rejects depth below near plane");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "depth_out_of_range") == 0,
               "sample coord reject label for depth out of range");

                   0.5f, 0.5f, 200.f, desc, camera, mapped, reason),
               "tryMap rejects depth above far plane");
               "above-far depth reports depth_out_of_range reason");

                   0.5f, 0.5f, 10.f, zeroDesc, camera, mapped, reason),
               "tryMap rejects empty froxel grid");
    expectTrue(reason == fuse::renderer::SampleCoordRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample coord reason");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(reason), "empty_grid") == 0,
               "sample coord reject label for empty grid");
    expectTrue(fuse::renderer::FroxelSliceLayout::isCameraValid(camera), "default camera is valid");

    fuse::renderer::FroxelCameraRejectReason cameraReason = fuse::renderer::FroxelCameraRejectReason::None;
    expectTrue(fuse::renderer::FroxelSliceLayout::tryValidateCamera(camera, cameraReason),
               "tryValidateCamera accepts valid camera");
    expectTrue(cameraReason == fuse::renderer::FroxelCameraRejectReason::None, "valid camera reports no reject reason");

    fuse::renderer::FroxelCameraDesc invalidNear{};
    invalidNear.nearPlane = 0.f;
    invalidNear.farPlane = 100.f;
    expectTrue(!fuse::renderer::FroxelSliceLayout::isCameraValid(invalidNear), "zero near plane is invalid");
    expectTrue(!fuse::renderer::FroxelSliceLayout::tryValidateCamera(invalidNear, cameraReason),
               "tryValidateCamera rejects zero near plane");
    expectTrue(cameraReason == fuse::renderer::FroxelCameraRejectReason::InvalidNearPlane,
               "zero near plane reports invalid_near_plane");
    expectTrue(std::strcmp(fuse::renderer::froxelCameraRejectReasonLabel(cameraReason), "invalid_near_plane") == 0,
               "invalid_near_plane camera reject reason label");

    fuse::renderer::FroxelCameraDesc invertedRange{};
    invertedRange.nearPlane = 100.f;
    invertedRange.farPlane = 1.f;
    expectTrue(!fuse::renderer::FroxelSliceLayout::tryValidateCamera(invertedRange, cameraReason),
               "tryValidateCamera rejects inverted depth range");
    expectTrue(cameraReason == fuse::renderer::FroxelCameraRejectReason::InvertedDepthRange,
               "inverted range reports inverted_depth_range");

    expectTrue(!fuse::renderer::FroxelSliceLayout::isSliceIndexOutOfRange(0u, desc),
               "origin slice index is in range");
    expectTrue(fuse::renderer::FroxelSliceLayout::isSliceIndexOutOfRange(99u, desc),
               "oversized slice index is out of range");

    fuse::renderer::FroxelGridRejectReason gridReason = fuse::renderer::FroxelGridRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryValidateFroxelGridDesc(desc, gridReason),
               "non-empty froxel desc validates");
    expectTrue(gridReason == fuse::renderer::FroxelGridRejectReason::None, "valid desc reports no grid reject reason");

    fuse::renderer::FroxelGridDesc zeroY{};
    zeroY.tilesX = 4;
    zeroY.tilesY = 0u;
    zeroY.slicesZ = 3;
    expectTrue(!fuse::renderer::froxel_util::tryValidateFroxelGridDesc(zeroY, gridReason),
               "zero tilesY fails grid desc validation");
    expectTrue(gridReason == fuse::renderer::FroxelGridRejectReason::EmptyTilesY,
               "zero tilesY reports empty_tiles_y");
    expectTrue(std::strcmp(fuse::renderer::froxelGridRejectReasonLabel(gridReason), "empty_tiles_y") == 0,
               "empty_tiles_y grid reject reason label");

    fuse::renderer::VolumetricFogParams params{};
    params.density = 0.02f;
    params.march_steps = 32u;
    expectTrue(!fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, camera, params),
               "valid populate inputs do not skip");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroY, camera, params),
               "empty grid desc skips populate");
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(desc, invertedRange, params),
               "invalid camera skips populate");

    fuse::renderer::FroxelScreenMappingRejectReason mappingReason =
        fuse::renderer::FroxelScreenMappingRejectReason::None;
    fuse::renderer::FroxelSampleCoords mappedCoords{};
                   0.5f, 0.5f, 10.f, desc, camera, mappedCoords, mappingReason),
               "tryMapScreenDepthToSampleCoords succeeds on valid inputs");
    expectTrue(mappingReason == fuse::renderer::FroxelScreenMappingRejectReason::None,
               "successful mapping reports no reject reason");

    fuse::renderer::FroxelSampleCoords rejectedCoords{};
    rejectedCoords.tileX0 = 99u;
                   0.5f, 0.5f, 0.01f, desc, camera, rejectedCoords, mappingReason),
    expectTrue(mappingReason == fuse::renderer::FroxelScreenMappingRejectReason::DepthBelowNear,
               "depth below near reports depth_below_near");
    expectTrue(std::strcmp(fuse::renderer::froxelScreenMappingRejectReasonLabel(mappingReason), "depth_below_near") ==
                   0,
               "depth_below_near screen mapping reject reason label");
    expectTrue(rejectedCoords.tileX0 == 99u, "failed mapping leaves sample coords untouched");

                   0.5f, 0.5f, 10.f, desc, invertedRange, rejectedCoords, mappingReason),
               "tryMap rejects invalid camera");
    expectTrue(mappingReason == fuse::renderer::FroxelScreenMappingRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera mapping reject reason");

    fuse::u32 mappedIndex = 0u;
    expectTrue(fuse::renderer::FroxelGridLayout::tryMapScreenDepthToFroxelIndex(
                   0.5f, 0.5f, 10.f, desc, camera, mappedIndex, mappingReason),
               "tryMapScreenDepthToFroxelIndex succeeds on valid inputs");
    expectTrue(mappedIndex < desc.froxelCount(), "mapped froxel index is in bounds");

    fuse::renderer::SampleCoordRejectReason coordReason = fuse::renderer::SampleCoordRejectReason::None;
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
    expectTrue(fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(inBounds, desc, coordReason),
               "in-bounds sample coords validate");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::None,
               "in-bounds coords report no sample reject reason");

    fuse::renderer::FroxelSampleCoords badTile = inBounds;
    badTile.tileX0 = 99u;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(badTile, desc, coordReason),
               "OOB tile coord fails sample validation");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::TileOutOfRange,
               "OOB tile reports tile_out_of_range");
    expectTrue(std::strcmp(fuse::renderer::sampleCoordRejectReasonLabel(coordReason), "tile_out_of_range") == 0,
               "tile_out_of_range sample coord reject reason label");

    fuse::renderer::FroxelSampleCoords badWeight = inBounds;
    badWeight.tx = 2.f;
    expectTrue(!fuse::renderer::FroxelGridLayout::tryValidateSampleCoords(badWeight, desc, coordReason),
               "OOB interpolation weight fails sample validation");
    expectTrue(coordReason == fuse::renderer::SampleCoordRejectReason::WeightOutOfRange,
               "OOB weight reports weight_out_of_range");

    expectTrue(fuse::renderer::froxel_util::canPopulateFromAnalyticFog(desc, camera, params),
               "valid populate inputs pass canPopulate preflight");

    fuse::renderer::PopulateRejectReason populateReason = fuse::renderer::PopulateRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, params, populateReason),
               "tryCanPopulate succeeds on valid inputs");
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::None,
               "valid populate reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::populateRejectReasonLabel(populateReason), "none") == 0,
               "none populate reject reason label");

    fuse::renderer::FroxelGridDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::froxel_util::shouldSkipFroxelPopulate(zeroDesc, camera, params),
               "empty froxel desc skips populate");
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(zeroDesc, camera, params, populateReason),
               "empty froxel desc fails populate preflight");
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::EmptyGrid,
               "empty froxel desc reports empty_grid populate reject reason");
    expectTrue(std::strcmp(fuse::renderer::populateRejectReasonLabel(populateReason), "empty_grid") == 0,
               "empty_grid populate reject reason label");

    fuse::renderer::FroxelCameraDesc badCamera{};
    badCamera.nearPlane = 100.f;
    badCamera.farPlane = 1.f;
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, badCamera, params, populateReason),
               "invalid camera fails populate preflight");
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera populate reject reason");

    fuse::renderer::VolumetricFogParams zeroDensity{};
    zeroDensity.density = 0.f;
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroDensity, populateReason),
               "zero density fails populate preflight");
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::ZeroDensity,
               "zero density reports zero_density populate reject reason");

    fuse::renderer::VolumetricFogParams zeroMarch = params;
    zeroMarch.march_steps = 0u;
    expectTrue(!fuse::renderer::froxel_util::tryCanPopulateFromAnalyticFog(desc, camera, zeroMarch, populateReason),
               "zero march steps fail populate preflight");
    expectTrue(populateReason == fuse::renderer::PopulateRejectReason::ZeroMarchSteps,
               "zero march steps report zero_march_steps populate reject reason");

    fuse::renderer::FroxelDensityGrid populated{};
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(populated, desc, camera, params),
               "tryPopulate succeeds on valid inputs");
    expectTrue(fuse::renderer::froxel_util::validatePopulatedDensity(populated, desc, params),
               "populated grid validates non-zero density");

    fuse::renderer::FroxelDensityGrid skipped{};
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(skipped, desc, camera, zeroDensity),
               "tryPopulate reports false when density fill is skipped");
    expectTrue(skipped.density.size() == desc.froxelCount(), "tryPopulate still allocates on skip path");
    expectTrue(fuse::renderer::froxel_util::validatePopulatedDensity(skipped, desc, zeroDensity),
               "zero-density populate validates allocated grid");

    fuse::renderer::FroxelDensityGrid grid{};
    grid.allocate(desc);
    grid.density[0] = 1.f;

    fuse::renderer::DensityLookupRejectReason lookupReason = fuse::renderer::DensityLookupRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanLookupAtIndexInRange(grid, desc, 0u, lookupReason),
               "origin index passes strict lookup preflight");
    expectTrue(!fuse::renderer::froxel_util::tryCanLookupAtIndexInRange(grid, desc, 999u, lookupReason),
               "OOB index fails strict lookup preflight");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::IndexOutOfRange,
               "OOB index reports index_out_of_range");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "index_out_of_range") == 0,
               "index_out_of_range lookup reject reason label");
    expectTrue(fuse::renderer::froxel_util::canLookupAtIndex(grid, desc, 999u),
               "legacy lookup preflight still ignores index");

    fuse::f32 strictSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityTrilinearInBounds(
                   grid, desc, inBounds, strictSample, lookupReason),
               "strict trilinear sample succeeds on in-bounds coords");
    expectNear(strictSample,
               fuse::renderer::froxel_util::sampleDensityTrilinear(grid, desc, inBounds),
               1e-5f,
               "strict trilinear sample matches unguarded sample");

    fuse::f32 rejectedStrict = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityTrilinearInBounds(
                   grid, desc, badWeight, rejectedStrict, lookupReason),
               "strict trilinear sample rejects OOB weights");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::SampleCoordsOutOfRange,
               "OOB weights report sample_coords_out_of_range");

    fuse::renderer::GridDensityRejectReason densityReason = fuse::renderer::GridDensityRejectReason::None;
    fuse::f32 indexSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtIndex(grid, desc, 0u, indexSample, lookupReason),
               "trySampleDensityAtIndex with reason succeeds on accessible grid");
    expectNear(indexSample, 1.f, 1e-5f, "trySampleDensityAtIndex with reason returns density");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful index sample reports no lookup reject reason");

    fuse::renderer::FroxelDensityGrid emptyGrid{};
    fuse::f32 rejectedIndexSample = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtIndex(emptyGrid, desc, 0u, rejectedIndexSample, lookupReason),
               "trySampleDensityAtIndex with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage lookup reject reason");
    expectTrue(std::strcmp(fuse::renderer::densityLookupRejectReasonLabel(lookupReason), "empty_storage") == 0,
               "empty_storage lookup reject reason label");

    fuse::f32 coordSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtCoord(grid, desc, 0u, 0u, 0u, coordSample, lookupReason),
               "trySampleDensityAtCoord with reason succeeds on accessible grid");
    expectNear(coordSample, 1.f, 1e-5f, "trySampleDensityAtCoord with reason returns density");

    expectTrue(fuse::renderer::froxel_util::tryWriteDensityAtIndex(grid, desc, 5u, 2.5f, lookupReason),
               "tryWriteDensityAtIndex with reason succeeds on accessible grid");
               "successful index write reports no lookup reject reason");

    fuse::renderer::FroxelGridDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 2;
    mismatched.slicesZ = 2;
    expectTrue(!fuse::renderer::froxel_util::tryValidateGridDensity(grid, mismatched, densityReason),
               "desc mismatch fails grid density validation");
    expectTrue(densityReason == fuse::renderer::GridDensityRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridDensityRejectReasonLabel(densityReason), "desc_mismatch") == 0,
               "desc_mismatch grid density reject reason label");

    fuse::renderer::FroxelDensityGrid populateGrid{};
    fuse::renderer::FroxelGridRejectReason populateGridReason = fuse::renderer::FroxelGridRejectReason::None;
    fuse::renderer::FroxelCameraRejectReason populateCameraReason = fuse::renderer::FroxelCameraRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(
                   populateGrid, desc, camera, params, populateGridReason, populateCameraReason),
               "tryPopulate succeeds on valid inputs");
    expectTrue(populateGrid.density.size() == desc.froxelCount(), "tryPopulate fills froxel grid");

    fuse::renderer::FroxelDensityGrid skippedGrid{};
    expectTrue(!fuse::renderer::froxel_util::tryPopulateFromAnalyticFog(
                   skippedGrid, zeroY, camera, params, populateGridReason, populateCameraReason),
               "tryPopulate rejects empty grid desc without modifying grid");
    expectTrue(populateGridReason == fuse::renderer::FroxelGridRejectReason::EmptyTilesY,
               "rejected populate reports empty_tiles_y");
    expectTrue(skippedGrid.density.empty(), "rejected populate leaves grid storage untouched");
    expectTrue(!fuse::renderer::froxel_util::tryWriteDensityAtCoord(grid, mismatched, 0u, 0u, 0u, 9.f, lookupReason),
               "tryWriteDensityAtCoord with reason rejects desc mismatch");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch lookup reject reason");

    fuse::renderer::ScreenMappingRejectReason mapReason = fuse::renderer::ScreenMappingRejectReason::None;
    fuse::f32 screenSample = 0.f;
    expectTrue(fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   grid, desc, camera, 0.25f, 0.25f, 3.16f, screenSample, mapReason, lookupReason),
               "trySampleDensityAtScreen with reasons succeeds on accessible grid");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::None,
               "successful screen sample reports no map reject reason");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::None,
               "successful screen sample reports no lookup reject reason");
    expectNear(screenSample,
               fuse::renderer::froxel_util::sampleDensityAtScreen(grid, desc, camera, 0.25f, 0.25f, 3.16f),
               1e-5f,
               "trySampleDensityAtScreen with reasons matches unguarded sample");

    fuse::f32 rejectedScreen = 1.f;
    expectTrue(!fuse::renderer::froxel_util::trySampleDensityAtScreen(
                   emptyGrid, desc, camera, 0.5f, 0.5f, 10.f, rejectedScreen, mapReason, lookupReason),
               "trySampleDensityAtScreen with reasons rejects empty storage");
               "lookup rejection leaves map reason unset");
    expectTrue(lookupReason == fuse::renderer::DensityLookupRejectReason::EmptyStorage,
               "empty storage screen sample reports empty_storage lookup reject reason");

                   grid, desc, camera, 0.5f, 0.5f, 0.01f, rejectedScreen, mapReason, lookupReason),
               "trySampleDensityAtScreen with reasons rejects depth below near plane");
    expectTrue(mapReason == fuse::renderer::ScreenMappingRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range map reject reason");

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
    expectTrue(fuse::renderer::froxel_util::canSampleAtCoordsStrict(grid, desc, inBounds),
               "in-bounds coords pass strict sample preflight");

    fuse::renderer::SampleCoordRejectReason sampleReason = fuse::renderer::SampleCoordRejectReason::None;
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoordsStrict(grid, desc, inBounds, sampleReason),
               "tryCanSampleAtCoordsStrict succeeds on valid coords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::None,
               "valid coords report no strict sample reject reason");

    fuse::renderer::FroxelSampleCoords warnCoords = inBounds;
    warnCoords.tx = 2.f;
    expectTrue(!fuse::renderer::froxel_util::canSampleAtCoordsStrict(grid, desc, warnCoords),
               "OOB weights fail strict sample preflight");
    expectTrue(fuse::renderer::froxel_util::tryCanSampleAtCoords(grid, desc, warnCoords, sampleReason),
               "non-strict preflight still succeeds when weights will be clamped");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::InvalidWeights,
               "non-strict preflight reports invalid_weights warning");
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoordsStrict(grid, desc, warnCoords, sampleReason),
               "strict preflight rejects OOB weights");
               "strict preflight reports invalid_weights reject reason");

    fuse::renderer::FroxelSampleCoords oobCoords = inBounds;
    oobCoords.tileX0 = 99u;
    expectTrue(!fuse::renderer::froxel_util::tryCanSampleAtCoordsStrict(grid, desc, oobCoords, sampleReason),
               "strict preflight rejects OOB tile coords");
    expectTrue(sampleReason == fuse::renderer::SampleCoordRejectReason::OutOfBounds,
               "OOB tile coords report out_of_bounds strict reject reason");
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

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady || bootstrap->device() == nullptr) {
        std::printf("SKIP: no Vulkan device — B5.11 pass-name checks only\n");
        return;
    }
#else
    std::printf("SKIP: Vulkan backend disabled — B5.11 pass-name checks only\n");
    return;
#endif

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready for B5.11 graph test");

    auto renderer = fuse::renderer::DeferredRenderer::create({});
    expectTrue(renderer->init(resources), "deferred renderer initialized for B5.11 graph test");

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
    testFroxelLookupRejectReasons();
    testFroxelSampleCoordRejectReasons();
    testFroxelTryClampSampleCoords();
    testFroxelTrySampleDensityGuards();
    testFroxelDensityAccessAndClampGuards();
    testFroxelDensityLookupAndSampleCoordGuards();
    testFroxelSampleCoordNormalizeAndScreenMappingGuards();
    testFroxelPopulatePreflightAndLookupGuards();
    testFroxelTrilinearAndWouldSkipGuards();
    testFroxelCoordLookupAndDiagnosticGuards();
    testFroxelClassifyPreflightAndIsBlockingGuards();
    testFroxelValidateGridDensityAndAccessGuards();
    testFroxelDensityAccessGuardsAndValidation();
    testFroxelMaxIndexDeepenHelpers();
    testFroxelSampleCoordGuards();
    testFroxelDensityAccessAndValidationDeepen();
    testFroxelTryClampAndDensityAccessGuards();
    testFroxelLookupAndSampleCoordGuards();
    testFroxelDensityLookupRejectReasons();
    testFroxelSampleCoordRejectAndEmptyGridGuards();
    testFroxelSampleCoordRejectAndPopulateGuards();
    testFroxelDensityLookupRejectAndValidationGuards();
    testFroxelSampleCoordNormalizeAndBuildGuards();
    testFroxelDensityLookupBoundsAndPreflightGuards();
    testFroxelDensityValidationAndSampleGuardDeepen();
    testFroxelDeepenGuardPreflights();
    testFroxelSampleCoordNormalizeAndPreflightGuards();
    testFroxelSampleCoordAndDensityPreflightGuards();
    testFroxelSampleCoordPreflightAndEmptyGridValidation();
    testFroxelSampleCoordMappingPreflightGuards();
    testFroxelSampleCoordBoundsPreflightGuards();
    testFroxelDensityLookupAndValidationPreflightGuards();
    testFroxelSampleCoordPreflightAndNormalizeGuards();
    testFroxelGridDensityValidationForDescGuards();
    testFroxelPreflightGuardsDeepen();
    testFroxelPreflightValidateAndLookupReasonGuards();
    testFroxelPopulatePreflightAndIndexValidationGuards();
    testFroxelPopulateLookupAndStrictSampleGuards();
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
