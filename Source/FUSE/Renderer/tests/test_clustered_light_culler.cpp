#include <fuse/core/init.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/deferred/deferred_renderer.hpp>
#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

void testClusterDescClampCounts() {
    fuse::renderer::ClusterDesc oversized{};
    oversized.tilesX = 999u;
    oversized.tilesY = 999u;
    oversized.slicesZ = 999u;
    oversized.maxLightsPerCluster = 999u;

    const fuse::renderer::ClusterDesc clamped = fuse::renderer::ClusterDesc::clampCounts(oversized);
    expectTrue(clamped.tilesX == fuse::renderer::ClusterDesc::kMaxTilesX, "tilesX clamped to max");
    expectTrue(clamped.tilesY == fuse::renderer::ClusterDesc::kMaxTilesY, "tilesY clamped to max");
    expectTrue(clamped.slicesZ == fuse::renderer::ClusterDesc::kMaxSlicesZ, "slicesZ clamped to max");
    expectTrue(clamped.maxLightsPerCluster == fuse::renderer::ClusterDesc::kMaxLightsPerCluster,
               "maxLightsPerCluster clamped to max");
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

    fuse::u32 oversizedTileX = 0u;
    fuse::u32 oversizedTileY = 0u;
    fuse::u32 oversizedSliceZ = 0u;
    fuse::renderer::ClusterGridLayout::decodeClusterIndex(999u, desc, oversizedTileX, oversizedTileY, oversizedSliceZ);
    expectTrue(oversizedTileX == 3u && oversizedTileY == 1u && oversizedSliceZ == 2u,
               "decode clamps oversized cluster index");

    expectTrue(fuse::renderer::ClusterGridLayout::clusterIndexClamped(99u, 99u, 99u, desc) == 23u,
               "clamped cluster index maps OOB coords to last cell");
    expectTrue(fuse::renderer::ClusterGridLayout::clusterIndexClamped(1u, 1u, 2u, desc) == 17u,
               "clamped cluster index preserves in-bounds coords");
    expectTrue(fuse::renderer::ClusterGridLayout::maxClusterIndex(desc) == 23u,
               "max cluster index matches last cell");
    expectTrue(fuse::renderer::ClusterGridLayout::clampClusterIndex(999u, desc) ==
                   fuse::renderer::ClusterGridLayout::maxClusterIndex(desc),
               "clamp cluster index agrees with max index");
}

void testClusterGridClampAndScreenMapping() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(fuse::renderer::ClusterGridLayout::clampClusterIndex(999u, desc) == 23u,
               "cluster index clamped to grid bounds");
    expectTrue(fuse::renderer::ClusterGridLayout::clampTileX(99u, desc) == 3u, "tile X clamp");
    expectTrue(fuse::renderer::ClusterGridLayout::clampTileY(99u, desc) == 1u, "tile Y clamp");
    expectTrue(fuse::renderer::ClusterGridLayout::clampSliceZ(99u, desc) == 2u, "slice Z clamp");
    expectTrue(fuse::renderer::ClusterGridLayout::clusterIndex(0u, 0u, 0u, desc) == 0u, "origin cluster index");
    expectTrue(fuse::renderer::ClusterGridLayout::clusterIndex(3u, 1u, 2u, desc) == 23u, "last cluster index");

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

    fuse::u32 aboveFar = 0u;
    expectTrue(!fuse::renderer::ClusterGridLayout::mapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 200.f, desc, camera, aboveFar),
               "depth above far plane rejected");

    fuse::u32 clampedScreen = 0u;
    expectTrue(fuse::renderer::ClusterGridLayout::mapScreenDepthToClusterIndex(
                   -0.5f, 1.5f, 10.f, desc, camera, clampedScreen),
               "out-of-range screen coords clamp to grid");
    expectTrue(clampedScreen < desc.clusterCount(), "clamped screen maps to valid cluster");
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

void testEmptyGridRebuild() {
    fuse::renderer::ClusterGridSoA grid{};
    grid.grid.resize(4u);
    grid.lightList = {0u, 1u, 2u};

    const fuse::u32 dropped = fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, 0u, {}, 4u);
    expectTrue(dropped == 0u, "zero cluster rebuild drops nothing");
    expectTrue(grid.grid.empty(), "zero cluster rebuild clears grid entries");
    expectTrue(grid.lightList.empty(), "zero cluster rebuild clears light list");
}

void testClusterUtilAssignmentCounts() {
    std::vector<fuse::u32> clusterLights;
    expectTrue(fuse::renderer::cluster_util::tryAssignLight(clusterLights, 0u, 2u), "first assign succeeds");
    expectTrue(fuse::renderer::cluster_util::tryAssignLight(clusterLights, 1u, 2u), "second assign succeeds");
    expectTrue(!fuse::renderer::cluster_util::tryAssignLight(clusterLights, 2u, 2u),
               "third assign rejected at capacity");
    expectTrue(clusterLights.size() == 2u, "cluster list stores assigned lights only");

    std::vector<fuse::u32> batchClusterLights;
    const std::vector<fuse::u32> candidates = {10u, 11u, 12u, 13u};
    const fuse::u32 batchDropped =
        fuse::renderer::cluster_util::assignLights(batchClusterLights, candidates, 2u);
    expectTrue(batchDropped == 2u, "batch assign drops overflow lights");
    expectTrue(batchClusterLights.size() == 2u, "batch assign stores capacity-limited lights");
    expectTrue(batchClusterLights[0] == 10u && batchClusterLights[1] == 11u, "batch assign preserves order");

    std::vector<std::vector<fuse::u32>> perClusterLights(2u);
    expectTrue(fuse::renderer::cluster_util::assignLightToCluster(perClusterLights, 0u, 7u, 2u),
               "assign light to cluster row succeeds");
    expectTrue(fuse::renderer::cluster_util::assignLightToCluster(perClusterLights, 0u, 8u, 2u),
               "second assign to same cluster succeeds");
    expectTrue(!fuse::renderer::cluster_util::assignLightToCluster(perClusterLights, 0u, 9u, 2u),
               "assign light to full cluster rejected");
    expectTrue(!fuse::renderer::cluster_util::assignLightToCluster(perClusterLights, 99u, 0u, 2u),
               "assign light to OOB cluster rejected");

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = 3u;
    const std::vector<std::vector<fuse::u32>> rebuiltClusterLights = {
        {0u, 2u},
        {},
        {1u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, rebuiltClusterLights);

    expectTrue(fuse::renderer::cluster_util::countAssignedLights(grid, clusterCount) == 3u,
               "assigned light count matches flat list");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClusters(grid, clusterCount) == 2u,
               "non-empty cluster count matches grid");
    expectTrue(fuse::renderer::cluster_util::countEmptyClusters(grid, clusterCount) == 1u,
               "empty cluster count matches grid");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCounts(grid, clusterCount),
               "population counts sum to cluster count");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulation(grid, clusterCount),
               "grid population includes contiguous offsets");
    expectTrue(fuse::renderer::cluster_util::clusterLightCount(grid, 0u) == 2u, "cluster light count for cluster 0");
    expectTrue(fuse::renderer::cluster_util::clusterLightCount(grid, 1u) == 0u, "cluster light count for empty cluster");
    expectTrue(fuse::renderer::cluster_util::clusterLightCount(grid, 99u) == 0u,
               "cluster light count OOB returns zero");

    std::vector<fuse::u32> cluster0Lights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLights(grid, 0u, cluster0Lights) == 2u,
               "lookup returns cluster light count");
    expectTrue(cluster0Lights.size() == 2u && cluster0Lights[0] == 0u && cluster0Lights[1] == 2u,
               "lookup copies cluster light indices");

    std::vector<fuse::u32> emptyClusterLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLights(grid, 1u, emptyClusterLights) == 0u,
               "lookup on empty cluster returns zero");
    expectTrue(emptyClusterLights.empty(), "lookup clears output for empty cluster");
}

void testClusterCapacityAndPopulationValidation() {
    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = 4u;
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {2u, 3u},
        {},
        {4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    expectTrue(fuse::renderer::cluster_util::countClustersAtCapacity(grid, clusterCount, 2u) == 2u,
               "two clusters at per-cluster capacity");
    expectTrue(fuse::renderer::cluster_util::countClustersAtCapacity(grid, clusterCount, 0u) == 0u,
               "zero max lights reports no clusters at capacity");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCounts(grid, clusterCount),
               "capacity-clamped grid preserves population invariant");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulation(grid, clusterCount),
               "capacity-clamped grid preserves contiguous offsets");

    fuse::renderer::ClusterGridSoA undersized{};
    undersized.grid.resize(2u);
    expectTrue(!fuse::renderer::cluster_util::validatePopulationCounts(undersized, clusterCount),
               "undersized grid fails population validation");
    expectTrue(!fuse::renderer::cluster_util::validateGridPopulation(undersized, clusterCount),
               "undersized grid fails grid population validation");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCounts(undersized, 0u),
               "zero cluster count is vacuously valid");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulation(undersized, 0u),
               "zero cluster count vacuously validates grid population");
}

void testClusterGridSoAAllocate() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 2;

    fuse::renderer::ClusterGridSoA grid{};
    grid.lightList = {99u};
    grid.allocate(desc);

    expectTrue(grid.aabbs.size() == desc.clusterCount(), "allocate sizes aabb storage");
    expectTrue(grid.grid.size() == desc.clusterCount(), "allocate sizes grid entries");
    expectTrue(grid.lightList.empty(), "allocate clears flat light list");
    expectTrue(!grid.isEmpty(), "allocated grid is non-empty");
    expectTrue(grid.matchesDesc(desc), "allocate matches desc");

    grid.clear();
    expectTrue(grid.isEmpty(), "cleared grid is empty");
    expectTrue(grid.aabbs.empty() && grid.grid.empty() && grid.lightList.empty(), "clear drops all storage");
    expectTrue(!grid.matchesDesc(desc), "cleared grid no longer matches desc");
}

void testClusterIndexClampAndGridGuards() {
    fuse::renderer::ClusterDesc layoutDesc{};
    layoutDesc.tilesX = 4;
    layoutDesc.tilesY = 2;
    layoutDesc.slicesZ = 3;

    expectTrue(!fuse::renderer::ClusterGridLayout::isEmptyGrid(layoutDesc),
               "non-zero cluster grid is not empty");
    expectTrue(fuse::renderer::ClusterGridLayout::isValidClusterIndex(0u, layoutDesc), "origin index is valid");
    expectTrue(fuse::renderer::ClusterGridLayout::isValidClusterIndex(23u, layoutDesc), "last index is valid");
    expectTrue(!fuse::renderer::ClusterGridLayout::isValidClusterIndex(24u, layoutDesc),
               "index at count is invalid");
    expectTrue(fuse::renderer::ClusterGridLayout::isClusterIndexOutOfRange(24u, layoutDesc),
               "index at count is out of range");
    expectTrue(!fuse::renderer::ClusterGridLayout::isClusterIndexOutOfRange(23u, layoutDesc),
               "last index is in range");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::ClusterGridLayout::isEmptyGrid(zeroDesc), "zero x dimension is empty grid");
    expectTrue(!fuse::renderer::ClusterGridLayout::isValidClusterIndex(0u, zeroDesc),
               "index 0 invalid on empty grid");
    expectTrue(fuse::renderer::ClusterGridLayout::isClusterIndexOutOfRange(0u, zeroDesc),
               "any index out of range on empty grid");
    expectTrue(fuse::renderer::ClusterGridLayout::maxClusterIndex(zeroDesc) == 0u,
               "max cluster index on empty grid is zero");

    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = desc.clusterCount();
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    expectTrue(fuse::renderer::cluster_util::gridMatchesDesc(grid, desc), "grid matches desc");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtIndex(grid, desc, 0u) == 2u,
               "count at origin index");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtIndex(grid, desc, 1u) == 0u,
               "count at empty cluster index");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtIndex(grid, desc, 3u) == 2u,
               "count at last index");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtIndex(grid, desc, 999u) == 2u,
               "count clamps OOB index to last cluster");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 2;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(!fuse::renderer::cluster_util::gridMatchesDesc(grid, mismatched),
               "grid does not match smaller desc");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtIndex(grid, mismatched, 0u) == 0u,
               "count rejects desc mismatch");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::gridMatchesDesc(emptyGrid, desc),
               "empty storage does not match desc");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtIndex(emptyGrid, desc, 0u) == 0u,
               "count on empty grid returns zero");

    expectTrue(fuse::renderer::cluster_util::countNonEmptyClusters(grid, clusterCount) == 3u,
               "non-empty cluster count after partial assignment");
    expectTrue(fuse::renderer::cluster_util::countEmptyClusters(grid, clusterCount) == 1u,
               "empty cluster count after partial assignment");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCounts(grid, clusterCount),
               "population counts sum after partial assignment");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulation(grid, clusterCount),
               "grid population valid after partial assignment");
}

void testClusterMaxIndexDeepenHelpers() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    expectTrue(desc.maxClusterIndex() == 23u, "ClusterDesc maxClusterIndex matches last cell");
    expectTrue(fuse::renderer::ClusterGridLayout::maxClusterIndex(desc) == desc.maxClusterIndex(),
               "ClusterGridLayout maxClusterIndex delegates to ClusterDesc");
    expectTrue(fuse::renderer::ClusteredLightCuller::maxClusterIndex(desc) == 23u,
               "ClusteredLightCuller maxClusterIndex delegates to grid layout");

    expectTrue(fuse::renderer::ClusterGridLayout::isAtMaxClusterIndex(23u, desc),
               "last cluster index is at max");
    expectTrue(!fuse::renderer::ClusterGridLayout::isAtMaxClusterIndex(22u, desc),
               "penultimate cluster index is not at max");
    expectTrue(!fuse::renderer::ClusterGridLayout::isAtMaxClusterIndex(23u, {}),
               "max index check rejects empty grid");

    fuse::u32 clampedIndex = 0u;
    expectTrue(fuse::renderer::ClusterGridLayout::tryClampClusterIndex(17u, desc, clampedIndex),
               "tryClamp succeeds on non-empty grid");
    expectTrue(clampedIndex == 17u, "tryClamp preserves in-bounds index");

    fuse::u32 clampedOob = 0u;
    expectTrue(fuse::renderer::ClusterGridLayout::tryClampClusterIndex(999u, desc, clampedOob),
               "tryClamp succeeds when clamping OOB index");
    expectTrue(clampedOob == desc.maxClusterIndex(), "tryClamp clamps OOB index to max");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::u32 emptyClamp = 99u;
    expectTrue(!fuse::renderer::ClusterGridLayout::tryClampClusterIndex(5u, zeroDesc, emptyClamp),
               "tryClamp rejects empty grid");
    expectTrue(emptyClamp == 0u, "tryClamp zeroes output on empty grid");
}

void testClusterLookupAtIndexGuards() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = desc.clusterCount();
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    std::vector<fuse::u32> originLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtIndex(grid, desc, 0u, originLights) == 2u,
               "lookup at origin index returns cluster count");
    expectTrue(originLights.size() == 2u && originLights[0] == 0u && originLights[1] == 1u,
               "lookup at origin copies assigned lights");

    std::vector<fuse::u32> clampedLastLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtIndex(grid, desc, 999u, clampedLastLights) == 2u,
               "lookup at OOB index clamps to last cluster");
    expectTrue(clampedLastLights.size() == 2u && clampedLastLights[0] == 3u && clampedLastLights[1] == 4u,
               "lookup at clamped last index copies assigned lights");

    std::vector<fuse::u32> emptyClusterLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtIndex(grid, desc, 1u, emptyClusterLights) == 0u,
               "lookup at empty cluster returns zero");
    expectTrue(emptyClusterLights.empty(), "lookup at empty cluster clears output");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    std::vector<fuse::u32> emptyStorageLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtIndex(emptyGrid, desc, 0u, emptyStorageLights) == 0u,
               "lookup on empty storage returns zero");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    std::vector<fuse::u32> mismatchedLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtIndex(grid, mismatched, 0u, mismatchedLights) == 0u,
               "lookup rejects desc mismatch");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    std::vector<fuse::u32> zeroGridLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtIndex(grid, zeroDesc, 0u, zeroGridLights) == 0u,
               "lookup rejects empty grid desc");

    expectTrue(fuse::renderer::cluster_util::canLookupAtIndex(grid, desc, 0u),
               "canLookup accepts matching grid and desc");
    expectTrue(fuse::renderer::cluster_util::canLookupAtIndex(grid, desc, 999u),
               "canLookup accepts OOB index that will be clamped");
    expectTrue(!fuse::renderer::cluster_util::canLookupAtIndex(emptyGrid, desc, 0u),
               "canLookup rejects empty storage");
    expectTrue(!fuse::renderer::cluster_util::canLookupAtIndex(grid, mismatched, 0u),
               "canLookup rejects desc mismatch");

    fuse::u32 tryCount = 0u;
    std::vector<fuse::u32> tryLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtIndex(grid, desc, 0u, tryLights, tryCount),
               "tryLookup succeeds on valid grid");
    expectTrue(tryCount == 2u, "tryLookup reports cluster light count");
    expectTrue(tryLights.size() == 2u && tryLights[0] == 0u && tryLights[1] == 1u,
               "tryLookup copies assigned lights");

    fuse::u32 tryEmptyCount = 0u;
    std::vector<fuse::u32> tryEmptyLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtIndex(grid, desc, 1u, tryEmptyLights, tryEmptyCount),
               "tryLookup succeeds on empty cluster");
    expectTrue(tryEmptyCount == 0u, "tryLookup reports zero for empty cluster");
    expectTrue(tryEmptyLights.empty(), "tryLookup clears output for empty cluster");

    fuse::u32 rejectedCount = 0u;
    std::vector<fuse::u32> rejectedLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtIndex(emptyGrid, desc, 0u, rejectedLights,
                                                                            rejectedCount),
               "tryLookup rejects empty storage");
    expectTrue(rejectedCount == 0u, "tryLookup zeroes count on guard failure");
    expectTrue(rejectedLights.empty(), "tryLookup clears output on guard failure");
}

void testClusterGridAccessibilityAndSkipGuards() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = desc.clusterCount();
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    expectTrue(fuse::renderer::cluster_util::isGridAccessible(grid, desc),
               "accessible grid matches non-empty desc");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipClusterLookup(grid, desc),
               "lookup not skipped on accessible grid");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipClusterCull(desc),
               "cull not skipped on non-empty desc");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterCull(zeroDesc),
               "cull skipped on empty desc");
    expectTrue(!fuse::renderer::cluster_util::isGridAccessible(grid, zeroDesc),
               "grid not accessible with empty desc");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLookup(grid, zeroDesc),
               "lookup skipped with empty desc");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::isGridAccessible(emptyGrid, desc),
               "empty storage is not accessible");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLookup(emptyGrid, desc),
               "lookup skipped on empty storage");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(!fuse::renderer::cluster_util::isGridAccessible(grid, mismatched),
               "mismatched desc is not accessible");
}

void testClusterLookupRejectReasons() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = desc.clusterCount();
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    fuse::renderer::ClusterLookupRejectReason reason = fuse::renderer::ClusterLookupRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryCanLookupAtIndex(grid, desc, 0u, reason),
               "tryCanLookup accepts accessible grid");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::None, "accessible grid reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(reason), "none") == 0,
               "lookup reject label for none");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtIndex(grid, zeroDesc, 0u, reason),
               "tryCanLookup rejects empty grid desc");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyGrid,
               "empty grid reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(reason), "empty_grid") == 0,
               "lookup reject label for empty grid");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtIndex(emptyGrid, desc, 0u, reason),
               "tryCanLookup rejects empty storage");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage reason");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtIndex(grid, mismatched, 0u, reason),
               "tryCanLookup rejects desc mismatch");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(reason), "desc_mismatch") == 0,
               "lookup reject label for desc mismatch");

    fuse::u32 tryCount = 0u;
    std::vector<fuse::u32> tryLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtIndex(grid, desc, 0u, tryLights, tryCount, reason),
               "tryLookup with reason succeeds on accessible grid");
    expectTrue(tryCount == 2u, "tryLookup with reason reports cluster light count");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::None,
               "successful lookup clears reject reason");

    fuse::u32 rejectedCount = 0u;
    std::vector<fuse::u32> rejectedLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtIndex(emptyGrid, desc, 0u, rejectedLights,
                                                                            rejectedCount, reason),
               "tryLookup with reason rejects empty storage");
    expectTrue(rejectedCount == 0u, "tryLookup with reason zeroes count on failure");
    expectTrue(rejectedLights.empty(), "tryLookup with reason clears output on failure");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "failed lookup preserves reject reason");
}

void testClusterDescScopedGridHelpers() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterGridSoA grid{};
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };

    const fuse::u32 dropped =
        fuse::renderer::ClusterLightGridLayout::rebuildLightGridForDesc(grid, desc, perClusterLights, 2u);
    expectTrue(dropped == 0u, "rebuildForDesc drops nothing when within capacity");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsetsForDesc(grid, desc),
               "validateContiguousOffsetsForDesc accepts rebuilt grid");
    expectTrue(fuse::renderer::cluster_util::countAssignedLightsForDesc(grid, desc) == 5u,
               "countAssignedLightsForDesc matches flat list");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(fuse::renderer::cluster_util::countAssignedLightsForDesc(grid, mismatched) == 0u,
               "countAssignedLightsForDesc rejects desc mismatch");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::validateContiguousOffsetsForDesc(grid, mismatched),
               "validateContiguousOffsetsForDesc rejects desc mismatch");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::ClusterGridSoA emptyGrid{};
    const fuse::u32 zeroDropped =
        fuse::renderer::ClusterLightGridLayout::rebuildLightGridForDesc(emptyGrid, zeroDesc, perClusterLights, 2u);
    expectTrue(zeroDropped == 0u, "rebuildForDesc early-outs on empty desc");
    expectTrue(emptyGrid.grid.empty() && emptyGrid.lightList.empty(),
               "rebuildForDesc clears storage on empty desc");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsetsForDesc(emptyGrid, zeroDesc),
               "validateContiguousOffsetsForDesc vacuously succeeds on empty desc");
    expectTrue(fuse::renderer::cluster_util::countAssignedLightsForDesc(emptyGrid, zeroDesc) == 0u,
               "countAssignedLightsForDesc zero on empty desc");

    fuse::renderer::GridPopulationRejectReason popReason = fuse::renderer::GridPopulationRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, desc, popReason),
               "tryValidateGridPopulationForDesc accepts rebuilt grid");
    expectTrue(popReason == fuse::renderer::GridPopulationRejectReason::None,
               "valid grid reports no population reject reason");
    expectTrue(fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(emptyGrid, zeroDesc, popReason),
               "tryValidateGridPopulationForDesc vacuously succeeds on empty desc");
    expectTrue(popReason == fuse::renderer::GridPopulationRejectReason::None,
               "empty desc reports no population reject reason");
    expectTrue(!fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, mismatched, popReason),
               "tryValidateGridPopulationForDesc rejects desc mismatch");
    expectTrue(popReason == fuse::renderer::GridPopulationRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reason");
}

void testClusterScreenMappingRejectReasons() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::u32 clusterIndex = 0u;
    fuse::renderer::ClusterScreenMappingRejectReason mapReason =
        fuse::renderer::ClusterScreenMappingRejectReason::None;
    expectTrue(fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, desc, camera, clusterIndex, mapReason),
               "tryMapScreenDepthToClusterIndex succeeds in range");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::None,
               "in-range mapping reports no reject reason");
    expectTrue(clusterIndex < desc.clusterCount(), "mapped cluster index in bounds");
    expectTrue(std::strcmp(fuse::renderer::clusterScreenMappingRejectReasonLabel(mapReason), "none") == 0,
               "screen mapping reject label for none");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::u32 emptyGridIndex = 0u;
    expectTrue(!fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, emptyGridIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects empty grid");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::EmptyGrid,
               "empty grid reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::clusterScreenMappingRejectReasonLabel(mapReason), "empty_grid") == 0,
               "screen mapping reject label for empty grid");

    fuse::renderer::ClusterCameraDesc invalidCamera = camera;
    invalidCamera.nearPlane = 0.f;
    expectTrue(!fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, desc, invalidCamera, clusterIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects invalid camera");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera reason");

    expectTrue(!fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 0.01f, desc, camera, clusterIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects depth below near plane");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::DepthOutOfRange,
               "depth below near plane reports depth_out_of_range reason");

    expectTrue(fuse::renderer::ClusterGridLayout::mapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, desc, camera, clusterIndex),
               "mapScreenDepthToClusterIndex delegates to tryMap on valid path");
}

void testLightGridRebuildPreflightGuards() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    const fuse::u32 clusterCount = desc.clusterCount();
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };

    fuse::renderer::ClusterGridSoA grid{};
    fuse::u32 dropped = 0u;
    fuse::renderer::LightGridRebuildRejectReason rebuildReason =
        fuse::renderer::LightGridRebuildRejectReason::None;

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount, rebuildReason),
               "tryCanRebuild accepts matching cluster count");
    expectTrue(rebuildReason == fuse::renderer::LightGridRebuildRejectReason::None,
               "matching count reports no rebuild reject reason");

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(
                   grid, desc, clusterCount, perClusterLights, 2u, dropped, rebuildReason),
               "tryRebuild succeeds on valid preflight");
    expectTrue(dropped == 0u, "tryRebuild reports zero dropped when within capacity");
    expectTrue(grid.lightList.size() == 5u, "tryRebuild packs flat light list");
    expectTrue(rebuildReason == fuse::renderer::LightGridRebuildRejectReason::None,
               "successful rebuild clears reject reason");

    fuse::renderer::ClusterGridSoA preserved{};
    preserved.grid.resize(clusterCount);
    preserved.lightList = {99u, 98u};
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(
                   preserved, desc, clusterCount + 1u, perClusterLights, 2u, dropped, rebuildReason),
               "tryRebuild rejects count mismatch without modifying grid");
    expectTrue(dropped == 0u, "rejected rebuild zeroes dropped count");
    expectTrue(preserved.lightList.size() == 2u, "rejected rebuild preserves existing light list");
    expectTrue(rebuildReason == fuse::renderer::LightGridRebuildRejectReason::CountMismatch,
               "count mismatch reports count_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::lightGridRebuildRejectReasonLabel(rebuildReason), "count_mismatch") == 0,
               "rebuild reject label for count mismatch");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, clusterCount, rebuildReason),
               "tryCanRebuild rejects non-zero count on empty desc");
    expectTrue(rebuildReason == fuse::renderer::LightGridRebuildRejectReason::EmptyGrid,
               "empty desc with non-zero count reports empty_grid reason");

    fuse::renderer::ClusterGridSoA zeroCountGrid{};
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, 0u, rebuildReason),
               "tryCanRebuild vacuously accepts zero cluster count");
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(
                   zeroCountGrid, desc, 0u, perClusterLights, 2u, dropped, rebuildReason),
               "tryRebuild vacuously succeeds on zero cluster count");
    expectTrue(zeroCountGrid.grid.empty() && zeroCountGrid.lightList.empty(),
               "zero-count tryRebuild clears storage");
}

void testClusterCoordLookupPreflightGuards() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = desc.clusterCount();
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    fuse::renderer::ClusterLookupRejectReason lookupReason = fuse::renderer::ClusterLookupRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryCanLookupAtCoord(grid, desc, 0u, 0u, 0u, lookupReason),
               "tryCanLookupAtCoord accepts accessible grid");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::None,
               "accessible coord lookup reports no reject reason");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u, lookupReason),
               "tryCanLookupAtCoord rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage reason for coord lookup");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtCoord(grid, zeroDesc, 0u, 0u, 0u, lookupReason),
               "tryCanLookupAtCoord rejects empty grid desc");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::EmptyGrid,
               "empty grid desc reports empty_grid reason for coord lookup");

    fuse::u32 tryCoordCount = 0u;
    std::vector<fuse::u32> tryCoordLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(
                   grid, desc, 0u, 0u, 0u, tryCoordLights, tryCoordCount, lookupReason),
               "tryLookupClusterLightsAtCoord with reason succeeds on accessible grid");
    expectTrue(tryCoordCount == 2u, "coord lookup with reason reports cluster light count");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::None,
               "successful coord lookup clears reject reason");

    fuse::u32 rejectedCoordCount = 0u;
    std::vector<fuse::u32> rejectedCoordLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(
                   emptyGrid, desc, 0u, 0u, 0u, rejectedCoordLights, rejectedCoordCount, lookupReason),
               "tryLookupClusterLightsAtCoord with reason rejects empty storage");
    expectTrue(rejectedCoordCount == 0u, "rejected coord lookup zeroes count");
    expectTrue(rejectedCoordLights.empty(), "rejected coord lookup clears output");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "failed coord lookup preserves reject reason");
}

void testClusterCoordLookupAndRebuildGuards() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = desc.clusterCount();
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, clusterCount),
               "canRebuild accepts matching cluster count");
    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, 0u),
               "canRebuild accepts zero cluster count");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, clusterCount + 1u),
               "canRebuild rejects oversized cluster count");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(zeroDesc, 4u),
               "canRebuild rejects non-zero count on empty desc");

    expectTrue(fuse::renderer::cluster_util::isLightGridAccessible(grid, desc),
               "rebuilt grid is light-grid accessible");
    expectTrue(fuse::renderer::cluster_util::hasAssignedLights(grid, clusterCount),
               "rebuilt grid has assigned lights");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::isLightGridAccessible(emptyGrid, desc),
               "empty storage is not light-grid accessible");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(emptyGrid, clusterCount),
               "empty storage has no assigned lights");

    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 0u, 0u, 0u) == 2u,
               "count at origin coord");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 1u, 0u, 0u) == 0u,
               "count at empty cluster coord");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 99u, 99u, 99u) == 2u,
               "count clamps OOB coords to last cluster");

    std::vector<fuse::u32> originCoordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, originCoordLights) ==
                   2u,
               "coord lookup at origin returns cluster count");
    expectTrue(originCoordLights.size() == 2u && originCoordLights[0] == 0u && originCoordLights[1] == 1u,
               "coord lookup copies assigned lights");

    std::vector<fuse::u32> clampedCoordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 99u, 99u, 99u, clampedCoordLights) ==
                   2u,
               "coord lookup clamps OOB coords");
    expectTrue(clampedCoordLights.size() == 2u && clampedCoordLights[0] == 3u && clampedCoordLights[1] == 4u,
               "coord lookup at clamped last cluster copies assigned lights");

    fuse::u32 tryCoordCount = 0u;
    std::vector<fuse::u32> tryCoordLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryCoordLights,
                                                                           tryCoordCount),
               "tryCoordLookup succeeds on accessible grid");
    expectTrue(tryCoordCount == 2u, "tryCoordLookup reports cluster light count");

    fuse::u32 rejectedCoordCount = 0u;
    std::vector<fuse::u32> rejectedCoordLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(emptyGrid, desc, 0u, 0u, 0u,
                                                                            rejectedCoordLights, rejectedCoordCount),
               "tryCoordLookup rejects empty storage");
    expectTrue(rejectedCoordCount == 0u, "tryCoordLookup zeroes count on guard failure");
    expectTrue(rejectedCoordLights.empty(), "tryCoordLookup clears output on guard failure");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, mismatched, 0u, 0u, 0u) == 0u,
               "coord count rejects desc mismatch");

    fuse::renderer::GridPopulationRejectReason popReason = fuse::renderer::GridPopulationRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, zeroDesc, popReason),
               "empty desc vacuously validates population");
    expectTrue(popReason == fuse::renderer::GridPopulationRejectReason::None,
               "empty desc reports no population reject reason");
    expectTrue(!fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, mismatched, popReason),
               "desc mismatch rejects population validation");
    expectTrue(popReason == fuse::renderer::GridPopulationRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::gridPopulationRejectReasonLabel(popReason), "desc_mismatch") == 0,
               "population reject label for desc mismatch");
}

void testValidateGridPopulationDeepen() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = desc.clusterCount();
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    fuse::renderer::GridPopulationRejectReason reason = fuse::renderer::GridPopulationRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryValidateGridPopulation(grid, clusterCount, reason),
               "tryValidate accepts rebuilt grid");
    expectTrue(reason == fuse::renderer::GridPopulationRejectReason::None, "valid grid reports no reject reason");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulationForDesc(grid, desc),
               "validateGridPopulationForDesc accepts matching desc");

    fuse::renderer::ClusterGridSoA undersized{};
    undersized.grid.resize(2u);
    expectTrue(!fuse::renderer::cluster_util::tryValidateGridPopulation(undersized, clusterCount, reason),
               "tryValidate rejects undersized grid");
    expectTrue(reason == fuse::renderer::GridPopulationRejectReason::UndersizedGrid,
               "undersized grid reports undersized reason");
    expectTrue(std::strcmp(fuse::renderer::gridPopulationRejectReasonLabel(reason), "undersized_grid") == 0,
               "reject reason label for undersized grid");

    fuse::renderer::ClusterGridSoA mismatchedCounts{};
    mismatchedCounts.grid.resize(clusterCount);
    mismatchedCounts.grid[0].count = 2u;
    mismatchedCounts.lightList = {0u};
    expectTrue(!fuse::renderer::cluster_util::tryValidateGridPopulation(mismatchedCounts, clusterCount, reason),
               "tryValidate rejects population mismatch");
    expectTrue(reason == fuse::renderer::GridPopulationRejectReason::PopulationMismatch,
               "population mismatch reports correct reason");

    fuse::renderer::ClusterGridSoA brokenOffsets{};
    brokenOffsets.grid.resize(clusterCount);
    brokenOffsets.grid[0] = {0u, 1u};
    brokenOffsets.grid[1] = {5u, 0u};
    brokenOffsets.lightList = {0u};
    expectTrue(!fuse::renderer::cluster_util::tryValidateGridPopulation(brokenOffsets, clusterCount, reason),
               "tryValidate rejects non-contiguous offsets");
    expectTrue(reason == fuse::renderer::GridPopulationRejectReason::NonContiguousOffsets,
               "broken offsets report contiguous reason");
    expectTrue(std::strcmp(fuse::renderer::gridPopulationRejectReasonLabel(reason), "non_contiguous_offsets") == 0,
               "reject reason label for broken offsets");

    fuse::renderer::ClusterDesc mismatchedDesc{};
    mismatchedDesc.tilesX = 1;
    mismatchedDesc.tilesY = 1;
    mismatchedDesc.slicesZ = 1;
    expectTrue(!fuse::renderer::cluster_util::validateGridPopulationForDesc(grid, mismatchedDesc),
               "validateGridPopulationForDesc rejects desc mismatch");
}

void testZeroDimensionClusterGrid() {
    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    zeroDesc.tilesY = 0u;
    zeroDesc.slicesZ = 0u;
    expectTrue(zeroDesc.isEmpty(), "zero tiles yields empty cluster desc");
    expectTrue(zeroDesc.clusterCount() == 0u, "zero-dimension cluster count is zero");

    fuse::u32 tileX = 99u;
    fuse::u32 tileY = 99u;
    fuse::u32 sliceZ = 99u;
    fuse::renderer::ClusterGridLayout::decodeClusterIndex(5u, zeroDesc, tileX, tileY, sliceZ);
    expectTrue(tileX == 0u && tileY == 0u && sliceZ == 0u, "decode on empty grid returns origin");

    expectTrue(fuse::renderer::ClusterGridLayout::clampClusterIndex(99u, zeroDesc) == 0u,
               "clamp cluster index on empty grid returns zero");
    expectTrue(fuse::renderer::ClusterGridLayout::clampTileX(99u, zeroDesc) == 0u, "clamp tile X on empty grid");
    expectTrue(fuse::renderer::ClusterGridLayout::clampTileY(99u, zeroDesc) == 0u, "clamp tile Y on empty grid");
    expectTrue(fuse::renderer::ClusterGridLayout::clampSliceZ(99u, zeroDesc) == 0u, "clamp slice Z on empty grid");
    expectTrue(fuse::renderer::ClusterGridLayout::clusterIndexClamped(99u, 99u, 99u, zeroDesc) == 0u,
               "clamped cluster index on empty grid returns origin");

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    fuse::u32 clusterIndex = 0u;
    expectTrue(!fuse::renderer::ClusterGridLayout::mapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, clusterIndex),
               "screen mapping rejects empty cluster grid");

    fuse::renderer::ClusterGridSoA grid{};
    grid.allocate(zeroDesc);
    expectTrue(grid.isEmpty(), "allocate on zero-dimension desc stays empty");
    expectTrue(grid.matchesDesc(zeroDesc), "empty grid matches zero desc");

    const fuse::u32 dropped =
        fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, 0u, {}, 4u);
    expectTrue(dropped == 0u, "zero cluster rebuild on empty grid drops nothing");
    expectTrue(fuse::renderer::cluster_util::countAssignedLights(grid, 0u) == 0u,
               "assignment count zero for empty grid");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClusters(grid, 0u) == 0u,
               "non-empty cluster count zero when cluster count is zero");
    expectTrue(fuse::renderer::cluster_util::countEmptyClusters(grid, 0u) == 0u,
               "empty cluster count zero when cluster count is zero");
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
    expectTrue(fuse::renderer::cluster_util::validateGridPopulation(culler.gridSoA(), desc.clusterCount()),
               "culler grid population valid after assign/skip cull");

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
    expectTrue(fuse::renderer::cluster_util::countAssignedLights(culler.gridSoA(), desc.clusterCount()) == 0u,
               "assignment count zero for empty scene");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClusters(culler.gridSoA(), desc.clusterCount()) == 0u,
               "non-empty cluster count zero for empty scene");
    expectTrue(fuse::renderer::cluster_util::countEmptyClusters(culler.gridSoA(), desc.clusterCount()) ==
                   desc.clusterCount(),
               "empty cluster count matches grid size");
    expectTrue(culler.gridSoA().matchesDesc(desc), "empty scene grid matches desc");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCounts(culler.gridSoA(), desc.clusterCount()),
               "empty scene population counts valid");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulation(culler.gridSoA(), desc.clusterCount()),
               "empty scene grid population valid");

    culler.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testCullerInitClampsOversizedDesc() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for oversized desc clamp test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::ClusterDesc oversized{};
    oversized.tilesX = 999u;
    oversized.tilesY = 999u;
    oversized.slicesZ = 999u;
    oversized.maxLightsPerCluster = 999u;

    fuse::renderer::ClusteredLightCuller culler;
    culler.init(oversized, resources);
    expectTrue(culler.isReady(), "oversized desc culler initializes");

    const fuse::renderer::ClusterDesc& clamped = culler.desc();
    expectTrue(clamped.tilesX == fuse::renderer::ClusterDesc::kMaxTilesX, "culler clamps tilesX on init");
    expectTrue(clamped.tilesY == fuse::renderer::ClusterDesc::kMaxTilesY, "culler clamps tilesY on init");
    expectTrue(clamped.slicesZ == fuse::renderer::ClusterDesc::kMaxSlicesZ, "culler clamps slicesZ on init");
    expectTrue(clamped.maxLightsPerCluster == fuse::renderer::ClusterDesc::kMaxLightsPerCluster,
               "culler clamps maxLightsPerCluster on init");
    expectTrue(culler.buffers().clusterCount == clamped.clusterCount(), "gpu cluster count matches clamped desc");
    expectTrue(culler.gridSoA().matchesDesc(clamped), "grid storage matches clamped desc");

    culler.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testEmptyGridRecordCullPassSkips() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for empty-grid record guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;

    fuse::renderer::ClusteredLightCuller culler;
    culler.init(zeroDesc, resources);
    expectTrue(culler.isReady(), "empty-grid culler initializes");

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 50.f;

    fuse::renderer::PointLightInput light{};
    light.position = {0.f, 0.f, -5.f};
    light.radius = 10.f;

    fuse::renderer::CommandBufferRecorder recorder;
    culler.recordCullPass(recorder, camera, {light}, {});
    expectTrue(culler.stats().cullPassCount == 0u, "empty grid skips recordCullPass");
    expectTrue(culler.stats().lightsCulled == 0u, "empty grid record leaves lights culled at zero");

    culler.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testZeroDimensionCuller() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for zero-dimension culler test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    zeroDesc.tilesY = 0u;
    zeroDesc.slicesZ = 0u;

    fuse::renderer::ClusteredLightCuller culler;
    culler.init(zeroDesc, resources);
    expectTrue(culler.isReady(), "zero-dimension culler initializes");
    expectTrue(culler.buffers().clusterCount == 0u, "zero-dimension gpu cluster count");

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 50.f;

    fuse::renderer::PointLightInput light{};
    light.position = {0.f, 0.f, -5.f};
    light.radius = 10.f;

    culler.cullLights({light}, {}, camera);
    expectTrue(culler.stats().lightsCulled == 0u, "zero-dimension cull assigns no lights");
    expectTrue(culler.stats().lightListEntries == 0u, "zero-dimension cull clears light list");
    expectTrue(culler.gridSoA().isEmpty(), "zero-dimension cull leaves empty grid");
    expectTrue(culler.gridSoA().matchesDesc(zeroDesc), "zero-dimension grid matches desc");

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
    expectTrue(fuse::renderer::cluster_util::clusterLightCount(culler.gridSoA(), 0u) == 2u,
               "cluster light count reflects capacity clamp");
    expectTrue(fuse::renderer::cluster_util::countClustersAtCapacity(culler.gridSoA(), desc.clusterCount(),
                                                                     desc.maxLightsPerCluster) == 1u,
               "capacity helper matches culler overflow stats");
    expectTrue(culler.stats().lightsDroppedOverflow == 3u, "excess intersecting lights dropped");
    expectTrue(culler.stats().clustersAtCapacity == 1u, "single cluster reported at capacity");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulation(culler.gridSoA(), desc.clusterCount()),
               "capacity-clamped cull preserves grid population");

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
    testClusterDescClampCounts();
    testClusterIndex();
    testClusterGridClampAndScreenMapping();
    testSliceDepthDistribution();
    testLightGridRebuildLayout();
    testLightGridRebuildOverflowClamp();
    testEmptyGridRebuild();
    testClusterUtilAssignmentCounts();
    testClusterCapacityAndPopulationValidation();
    testClusterGridSoAAllocate();
    testClusterIndexClampAndGridGuards();
    testClusterMaxIndexDeepenHelpers();
    testClusterLookupAtIndexGuards();
    testClusterGridAccessibilityAndSkipGuards();
    testClusterLookupRejectReasons();
    testClusterDescScopedGridHelpers();
    testClusterScreenMappingRejectReasons();
    testLightGridRebuildPreflightGuards();
    testClusterCoordLookupPreflightGuards();
    testClusterCoordLookupAndRebuildGuards();
    testValidateGridPopulationDeepen();
    testZeroDimensionClusterGrid();
    testCullerInitClampsOversizedDesc();
    testEmptyGridRecordCullPassSkips();
    testCullerInitAndClusterBuild();
    testLightCullAssignsAndSkips();
    testEmptySceneCull();
    testZeroDimensionCuller();
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
