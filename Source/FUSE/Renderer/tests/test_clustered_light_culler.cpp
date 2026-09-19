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
void testClusterCoordValidationAndTryIndex() {
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
    expectTrue(fuse::renderer::ClusterGridLayout::isValidClusterCoords(0u, 0u, 0u, desc),
               "origin coords are valid");
    expectTrue(fuse::renderer::ClusterGridLayout::isValidClusterCoords(3u, 1u, 2u, desc),
               "last cell coords are valid");
    expectTrue(!fuse::renderer::ClusterGridLayout::isValidClusterCoords(4u, 0u, 0u, desc),
               "tile X at count is invalid");
    expectTrue(!fuse::renderer::ClusterGridLayout::isValidClusterCoords(0u, 2u, 0u, desc),
               "tile Y at count is invalid");
    expectTrue(!fuse::renderer::ClusterGridLayout::isValidClusterCoords(0u, 0u, 3u, desc),
               "slice Z at count is invalid");
    expectTrue(fuse::renderer::ClusterGridLayout::isClusterCoordsOutOfRange(99u, 99u, 99u, desc),
               "oversized coords are out of range");
    expectTrue(!fuse::renderer::ClusterGridLayout::isClusterCoordsOutOfRange(1u, 1u, 1u, desc),
               "in-bounds coords are not out of range");

    fuse::u32 outIndex = 0u;
    expectTrue(fuse::renderer::ClusterGridLayout::tryClusterIndex(1u, 1u, 2u, desc, outIndex),
               "tryClusterIndex succeeds for in-bounds coords");
    expectTrue(outIndex == 17u, "tryClusterIndex encodes expected flat index");
    expectTrue(!fuse::renderer::ClusterGridLayout::tryClusterIndex(99u, 99u, 99u, desc, outIndex),
               "tryClusterIndex rejects OOB coords");

    expectTrue(!fuse::renderer::ClusterGridLayout::isValidClusterCoords(0u, 0u, 0u, zeroDesc),
               "coords invalid on empty grid");
    expectTrue(fuse::renderer::ClusterGridLayout::isClusterCoordsOutOfRange(0u, 0u, 0u, zeroDesc),
               "coords out of range on empty grid");
    expectTrue(!fuse::renderer::ClusterGridLayout::tryClusterIndex(0u, 0u, 0u, zeroDesc, outIndex),
               "tryClusterIndex rejects empty grid");
}

void testStrictLookupAndPopulationHelpers() {
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

    std::vector<fuse::u32> strictLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLights(grid, desc, 0u, strictLights),
               "strict lookup succeeds for valid index");
    expectTrue(strictLights.size() == 2u && strictLights[0] == 0u && strictLights[1] == 1u,
               "strict lookup copies assigned lights");

    std::vector<fuse::u32> staleOutput = {99u};
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLights(grid, desc, 99u, staleOutput),
               "strict lookup rejects OOB index without clamping");
    expectTrue(staleOutput.empty(), "strict lookup clears output on OOB rejection");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    std::vector<fuse::u32> emptyLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLights(emptyGrid, desc, 0u, emptyLights),
               "strict lookup rejects empty storage");

    expectTrue(fuse::renderer::cluster_util::hasAssignedLights(grid, clusterCount),
               "hasAssignedLights true when lights present");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(grid, 0u),
               "hasAssignedLights false for zero cluster count");
    expectTrue(fuse::renderer::cluster_util::countAssignedLightsWithDesc(grid, desc) == 5u,
               "countAssignedLightsWithDesc matches flat total");
    expectTrue(fuse::renderer::cluster_util::countAssignedLightsWithDesc(emptyGrid, desc) == 0u,
               "countAssignedLightsWithDesc zero on empty storage");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulationWithDesc(grid, desc),
               "validateGridPopulationWithDesc passes for rebuilt grid");
    expectTrue(!fuse::renderer::cluster_util::validateGridPopulationWithDesc(emptyGrid, desc),
               "validateGridPopulationWithDesc rejects empty storage");

    fuse::renderer::ClusterGridSoA zeroGrid{};
    expectTrue(fuse::renderer::cluster_util::validateGridPopulationWithDesc(zeroGrid, zeroDesc),
               "validateGridPopulationWithDesc vacuously true for empty grid");

void testLightListBoundsValidation() {
    const fuse::u32 clusterCount = 2u;
    grid.grid.resize(clusterCount);
    grid.grid[0] = {0u, 2u};
    grid.grid[1] = {2u, 1u};
    grid.lightList = {0u, 1u, 2u};

    expectTrue(fuse::renderer::ClusterLightGridLayout::validateLightListBounds(grid, clusterCount),
               "valid grid passes light list bounds check");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsets(grid, clusterCount),
               "valid grid passes contiguous offsets");

    fuse::renderer::ClusterGridSoA corrupt{};
    corrupt.grid.resize(clusterCount);
    corrupt.grid[0] = {2u, 2u};
    corrupt.grid[1] = {4u, 0u};
    corrupt.lightList = {0u, 1u, 2u};

    expectTrue(!fuse::renderer::ClusterLightGridLayout::validateLightListBounds(corrupt, clusterCount),
               "light list bounds rejects entry past list end");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::validateContiguousOffsets(corrupt, clusterCount),
               "contiguous offsets also reject misaligned packing");
    expectTrue(!fuse::renderer::cluster_util::validateGridPopulation(corrupt, clusterCount),
               "grid population rejects corrupt light list bounds");

    expectTrue(fuse::renderer::ClusterLightGridLayout::validateLightListBounds(grid, 0u),
               "zero cluster count vacuously validates light list bounds");
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
void testClusterLightGridAccessibilityGuards() {
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

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtIndex(grid, zeroDesc, 0u, reason),
               "tryCanLookup rejects empty grid desc");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyGrid,
               "empty grid reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(reason), "empty_grid") == 0,
               "lookup reject label for empty grid");

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtIndex(emptyGrid, desc, 0u, reason),
               "tryCanLookup rejects empty storage");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage reason");

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
               "failed lookup preserves reject reason");

void testClusterDirectLookupGuards() {
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

    expectTrue(fuse::renderer::cluster_util::canLookupCluster(grid, 0u),
               "canLookup accepts in-range cluster index");
    expectTrue(!fuse::renderer::cluster_util::canLookupCluster(grid, 99u),
               "canLookup rejects OOB cluster index");
    expectTrue(!fuse::renderer::cluster_util::canLookupCluster({}, 0u),
               "canLookup rejects empty storage");

    fuse::renderer::ClusterLookupRejectReason reason = fuse::renderer::ClusterLookupRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryCanLookupCluster(grid, 0u, reason),
               "tryCanLookup accepts in-range cluster index");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::None,
               "in-range cluster reports no reject reason");

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupCluster(grid, 99u, reason),
               "tryCanLookup rejects OOB cluster index");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::OutOfRangeCluster,
               "OOB cluster reports out_of_range_cluster reason");
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(reason), "out_of_range_cluster") == 0,
               "lookup reject label for OOB cluster");

    fuse::u32 tryCount = 0u;
    std::vector<fuse::u32> tryLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLights(grid, 0u, tryLights, tryCount),
               "tryLookupCluster succeeds on in-range cluster");
    expectTrue(tryCount == 2u, "tryLookupCluster reports cluster light count");
    expectTrue(tryLights.size() == 2u && tryLights[0] == 0u && tryLights[1] == 1u,
               "tryLookupCluster copies assigned lights");

    fuse::u32 emptyCount = 0u;
    std::vector<fuse::u32> emptyLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLights(grid, 1u, emptyLights, emptyCount),
               "tryLookupCluster succeeds on empty cluster row");
    expectTrue(emptyCount == 0u, "tryLookupCluster reports zero for empty cluster");
    expectTrue(emptyLights.empty(), "tryLookupCluster clears output for empty cluster");

    fuse::u32 rejectedCount = 0u;
    std::vector<fuse::u32> rejectedLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLights({}, 0u, rejectedLights, rejectedCount, reason),
               "tryLookupCluster rejects empty storage");
    expectTrue(rejectedCount == 0u, "tryLookupCluster zeroes count on guard failure");
    expectTrue(rejectedLights.empty(), "tryLookupCluster clears output on guard failure");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage reason");

    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLights(grid, 99u, rejectedLights, rejectedCount, reason),
               "tryLookupCluster rejects OOB cluster index");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::OutOfRangeCluster,
               "OOB cluster lookup preserves reject reason");
}

void testClusterPopulationStateGuards() {
    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = 4u;
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    expectTrue(fuse::renderer::cluster_util::hasAssignedLights(grid, clusterCount),
               "partially populated grid has assigned lights");
    expectTrue(!fuse::renderer::cluster_util::isGridPopulationEmpty(grid, clusterCount),
               "partially populated grid is not population-empty");

    fuse::renderer::ClusterGridSoA emptyPopulation{};
    emptyPopulation.grid.resize(clusterCount);
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(emptyPopulation, clusterCount),
               "all-empty grid has no assigned lights");
    expectTrue(fuse::renderer::cluster_util::isGridPopulationEmpty(emptyPopulation, clusterCount),
               "all-empty grid is population-empty");
    expectTrue(fuse::renderer::cluster_util::isGridPopulationEmpty(emptyPopulation, 0u),
               "zero cluster count is vacuously population-empty");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(emptyPopulation, 0u),
               "zero cluster count has no assigned lights");
}

void testClusterDescScopedPopulationCounts() {
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
    fuse::renderer::ClusterLightGridLayout::rebuildLightGridForDesc(grid, desc, perClusterLights, 2u);

    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, desc) == 3u,
               "countNonEmptyClustersForDesc matches non-empty rows");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, desc) == 1u,
               "countEmptyClustersForDesc matches empty rows");
    expectTrue(fuse::renderer::cluster_util::countClustersAtCapacityForDesc(grid, desc, 2u) == 2u,
               "countClustersAtCapacityForDesc matches capped rows");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, mismatched) == 0u,
               "countNonEmptyClustersForDesc rejects desc mismatch");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, mismatched) == 0u,
               "countEmptyClustersForDesc rejects desc mismatch");
    expectTrue(fuse::renderer::cluster_util::countClustersAtCapacityForDesc(grid, mismatched, 2u) == 0u,
               "countClustersAtCapacityForDesc rejects desc mismatch");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(emptyGrid, zeroDesc) == 0u,
               "countNonEmptyClustersForDesc zero on empty desc");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(emptyGrid, zeroDesc) == 0u,
               "countEmptyClustersForDesc zero on empty desc");
    expectTrue(fuse::renderer::cluster_util::countClustersAtCapacityForDesc(emptyGrid, zeroDesc, 2u) == 0u,
               "countClustersAtCapacityForDesc zero on empty desc");
}

void testClusterLightGridRebuildSkipGuard() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc),
               "non-empty desc does not skip rebuild");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(zeroDesc),
               "empty desc skips light-grid rebuild");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(zeroDesc) ==
                   fuse::renderer::cluster_util::shouldSkipClusterCull(zeroDesc),
               "rebuild skip agrees with cull skip on empty desc");
}

void testClusterDescScopedGridHelpers() {


    const fuse::u32 dropped =
        fuse::renderer::ClusterLightGridLayout::rebuildLightGridForDesc(grid, desc, perClusterLights, 2u);
    expectTrue(dropped == 0u, "rebuildForDesc drops nothing when within capacity");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsetsForDesc(grid, desc),
               "validateContiguousOffsetsForDesc accepts rebuilt grid");
    expectTrue(fuse::renderer::cluster_util::countAssignedLightsForDesc(grid, desc) == 5u,
               "countAssignedLightsForDesc matches flat list");

    expectTrue(fuse::renderer::cluster_util::countAssignedLightsForDesc(grid, mismatched) == 0u,
               "countAssignedLightsForDesc rejects desc mismatch");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::validateContiguousOffsetsForDesc(grid, mismatched),
               "validateContiguousOffsetsForDesc rejects desc mismatch");

    const fuse::u32 zeroDropped =
        fuse::renderer::ClusterLightGridLayout::rebuildLightGridForDesc(emptyGrid, zeroDesc, perClusterLights, 2u);
    expectTrue(zeroDropped == 0u, "rebuildForDesc early-outs on empty desc");
    expectTrue(emptyGrid.grid.empty() && emptyGrid.lightList.empty(),
               "rebuildForDesc clears storage on empty desc");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsetsForDesc(emptyGrid, zeroDesc),
               "validateContiguousOffsetsForDesc vacuously succeeds on empty desc");
    expectTrue(fuse::renderer::cluster_util::countAssignedLightsForDesc(emptyGrid, zeroDesc) == 0u,
               "countAssignedLightsForDesc zero on empty desc");
    expectTrue(fuse::renderer::cluster_util::isLightGridAccessible(grid, desc),
               "rebuilt grid is accessible");
               "accessible grid does not skip lookup");
    expectTrue(fuse::renderer::cluster_util::hasAssignedLights(grid, clusterCount),
               "rebuilt grid has assigned lights");

    expectTrue(!fuse::renderer::cluster_util::isLightGridAccessible(grid, mismatched),
               "inaccessible when desc mismatches storage");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLookup(grid, mismatched),
               "desc mismatch skips cluster lookup");

    expectTrue(!fuse::renderer::cluster_util::isLightGridAccessible(grid, zeroDesc),
               "zero-dimension desc is not accessible");
               "empty desc skips cluster lookup");
    fuse::renderer::GridPopulationRejectReason zeroPopReason = fuse::renderer::GridPopulationRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, zeroDesc, zeroPopReason),
               "empty desc vacuously validates grid population");
    expectTrue(zeroPopReason == fuse::renderer::GridPopulationRejectReason::None,
               "empty desc reports no population reject reason");

    expectTrue(!fuse::renderer::cluster_util::isLightGridAccessible(emptyGrid, desc),
               "empty storage skips cluster lookup");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(emptyGrid, clusterCount),
               "empty storage has no assigned lights");

    fuse::renderer::ClusterLookupRejectReason lookupReason = fuse::renderer::ClusterLookupRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryCanLookupAtIndex(grid, desc, 0u, lookupReason),
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::None,
               "accessible lookup reports no reject reason");

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtIndex(emptyGrid, desc, 0u, lookupReason),
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(lookupReason), "empty_storage") == 0,
               "lookup reject label for empty storage");

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtIndex(grid, mismatched, 0u, lookupReason),
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::DescMismatch,

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtIndex(grid, zeroDesc, 0u, lookupReason),
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::EmptyGrid,
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(lookupReason), "empty_grid") == 0,

    std::vector<fuse::u32> coordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, coordLights) == 2u,
               "coord lookup returns cluster count");
    expectTrue(coordLights.size() == 2u && coordLights[0] == 0u && coordLights[1] == 1u,
               "coord lookup copies assigned lights");

    std::vector<fuse::u32> clampedCoordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 99u, 99u, 99u,
                                                                         clampedCoordLights) == 2u,
               "coord lookup clamps OOB tile/slice to last cluster");
    expectTrue(clampedCoordLights.size() == 2u && clampedCoordLights[0] == 3u && clampedCoordLights[1] == 4u,
               "clamped coord lookup copies last cluster lights");

    std::vector<fuse::u32> rejectedCoordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(emptyGrid, desc, 0u, 0u, 0u,
                                                                        rejectedCoordLights) == 0u,
               "coord lookup rejects inaccessible grid");
    expectTrue(rejectedCoordLights.empty(), "coord lookup clears output on guard failure");

    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, clusterCount),
               "rebuild allowed when cluster count matches desc");
    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, 0u),
               "zero cluster rebuild is always allowed");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(zeroDesc, clusterCount),
               "rebuild rejected when desc is empty but cluster count is non-zero");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, clusterCount + 1u),
               "rebuild rejected when cluster count mismatches desc");

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

void testClusterRebuildPreflightGuards() {
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

    fuse::renderer::GridRebuildRejectReason rebuildReason = fuse::renderer::GridRebuildRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount, rebuildReason),
               "tryCanRebuild accepts matching cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::None,
               "matching count reports no rebuild reject reason");
    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGridForDesc(desc),
               "canRebuildForDesc accepts non-empty desc");
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGridForDesc(desc, rebuildReason),
               "tryCanRebuildForDesc accepts non-empty desc");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, clusterCount),
               "rebuild not skipped on valid desc/count");

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, 0u, rebuildReason),
               "tryCanRebuild accepts zero cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::None,
               "zero cluster count reports no rebuild reject reason");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount + 1u, rebuildReason),
               "tryCanRebuild rejects oversized cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::CountMismatch,
               "oversized count reports count_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "count_mismatch") == 0,
               "rebuild reject label for count mismatch");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, 4u, rebuildReason),
               "tryCanRebuild rejects non-zero count on empty desc");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::EmptyGrid,
               "empty desc reports empty_grid rebuild reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "empty_grid") == 0,
               "rebuild reject label for empty grid");
    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGridForDesc(zeroDesc),
               "canRebuildForDesc accepts empty desc with zero cluster count");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(zeroDesc, 4u),
               "rebuild skipped on empty desc with non-zero count");

    fuse::renderer::ClusterGridSoA grid{};
    grid.grid.resize(clusterCount);
    grid.lightList = {99u};
    const fuse::u32 preservedListSize = static_cast<fuse::u32>(grid.lightList.size());

    const fuse::u32 rejectedDropped = fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(
        grid, desc, clusterCount + 1u, perClusterLights, 2u);
    expectTrue(rejectedDropped == 0u, "tryRebuild returns zero when preflight rejects");
    expectTrue(grid.lightList.size() == preservedListSize, "tryRebuild leaves grid unchanged on reject");

    const fuse::u32 acceptedDropped =
        fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(grid, desc, clusterCount, perClusterLights, 2u);
    expectTrue(acceptedDropped == 0u, "tryRebuild succeeds on valid preflight");
    expectTrue(fuse::renderer::cluster_util::countAssignedLights(grid, clusterCount) == 5u,
               "tryRebuild packs assigned lights");

    fuse::renderer::ClusterGridSoA descScopedGrid{};
    const fuse::u32 descScopedDropped = fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(
        descScopedGrid, desc, perClusterLights, 2u);
    expectTrue(descScopedDropped == 0u, "tryRebuildForDesc succeeds on valid desc");
    expectTrue(fuse::renderer::ClusterLightGridLayout::validateContiguousOffsetsForDesc(descScopedGrid, desc),
               "tryRebuildForDesc produces contiguous offsets");

    fuse::renderer::ClusterGridSoA rejectedDescGrid{};
    rejectedDescGrid.lightList = {42u};
    const fuse::u32 clearedDescDropped = fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(
        rejectedDescGrid, zeroDesc, perClusterLights, 2u);
    expectTrue(clearedDescDropped == 0u, "tryRebuildForDesc returns zero on empty desc");
    expectTrue(rejectedDescGrid.grid.empty() && rejectedDescGrid.lightList.empty(),
               "tryRebuildForDesc clears storage on empty desc");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    fuse::renderer::ClusterGridSoA mismatchedGrid{};
    mismatchedGrid.lightList = {42u};
    const fuse::u32 preservedListSizeMismatched = static_cast<fuse::u32>(mismatchedGrid.lightList.size());
    const fuse::u32 rejectedDescDropped = fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(
        mismatchedGrid, mismatched, desc.clusterCount(), perClusterLights, 2u);
    expectTrue(rejectedDescDropped == 0u, "tryRebuild returns zero when desc/count mismatch rejects");
    expectTrue(mismatchedGrid.lightList.size() == preservedListSizeMismatched,
               "tryRebuild leaves grid unchanged on desc/count mismatch reject");
}

void testClusterContiguousOffsetPreflight() {
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
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    fuse::renderer::GridPopulationRejectReason offsetReason = fuse::renderer::GridPopulationRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsets(grid, clusterCount, offsetReason),
               "tryValidateContiguousOffsets accepts rebuilt grid");
    expectTrue(offsetReason == fuse::renderer::GridPopulationRejectReason::None,
               "valid offsets report no reject reason");

    fuse::renderer::ClusterGridSoA undersized{};
    undersized.grid.resize(2u);
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsets(undersized, clusterCount,
                                                                                     offsetReason),
               "tryValidateContiguousOffsets rejects undersized grid");
    expectTrue(offsetReason == fuse::renderer::GridPopulationRejectReason::UndersizedGrid,
               "undersized grid reports undersized reason");

    fuse::renderer::ClusterGridSoA brokenOffsets{};
    brokenOffsets.grid.resize(clusterCount);
    brokenOffsets.grid[0] = {0u, 1u};
    brokenOffsets.grid[1] = {5u, 0u};
    brokenOffsets.lightList = {0u};
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsets(brokenOffsets, clusterCount,
                                                                                     offsetReason),
               "tryValidateContiguousOffsets rejects non-contiguous offsets");
    expectTrue(offsetReason == fuse::renderer::GridPopulationRejectReason::NonContiguousOffsets,
               "broken offsets report non_contiguous_offsets reason");
}

void testClusterCoordLookupRejectReasons() {
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
    expectTrue(fuse::renderer::cluster_util::tryCanLookupAtCoord(grid, desc, 0u, 0u, 0u, reason),
               "tryCanLookupAtCoord accepts accessible grid");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::None, "accessible coord lookup reports no reason");
    expectTrue(fuse::renderer::cluster_util::canLookupAtCoord(grid, desc, 0u, 0u, 0u),
               "canLookupAtCoord accepts accessible grid");

    fuse::u32 tryCount = 0u;
    std::vector<fuse::u32> tryLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryLights, tryCount,
                                                                          reason),
               "tryCoordLookup with reason succeeds on accessible grid");
    expectTrue(tryCount == 2u, "tryCoordLookup with reason reports cluster light count");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::None,
               "successful coord lookup clears reject reason");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    fuse::u32 rejectedCount = 0u;
    std::vector<fuse::u32> rejectedLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(emptyGrid, desc, 0u, 0u, 0u, rejectedLights,
                                                                          rejectedCount, reason),
               "tryCoordLookup with reason rejects empty storage");
    expectTrue(rejectedCount == 0u, "tryCoordLookup with reason zeroes count on failure");
    expectTrue(rejectedLights.empty(), "tryCoordLookup with reason clears output on failure");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "failed coord lookup preserves reject reason");
}

void testClusterPopulationSkipAndDescHelpers() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::cluster_util::shouldSkipLightGridPopulation(zeroDesc),
               "population validation skipped on empty desc");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipLightGridPopulation(desc),
               "population validation not skipped on non-empty desc");

    fuse::renderer::ClusterGridSoA grid{};
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGridForDesc(grid, desc, perClusterLights, 2u);

    expectTrue(fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, desc),
               "hasAssignedLightsForDesc true when lights assigned");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, zeroDesc),
               "hasAssignedLightsForDesc false on empty desc");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, mismatched),
               "hasAssignedLightsForDesc false on desc mismatch");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(emptyGrid, desc),
               "hasAssignedLightsForDesc false on empty storage");
}

void testClusterCoordLookupAndRebuildGuards() {
    expectTrue(std::strcmp(fuse::renderer::gridPopulationRejectReasonLabel(popReason), "desc_mismatch") == 0,
               "population reject label for desc mismatch");

void testClusterLookupAtCoordGuards() {

void testClusterCoordLookupRebuildAndPopulationGuards() {
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
    expectTrue(fuse::renderer::cluster_util::canLookupAtCoord(grid, desc, 0u, 0u, 0u),
               "canLookupAtCoord accepts accessible grid");

    fuse::renderer::ClusterGridSoA emptyStorage{};
    expectTrue(!fuse::renderer::cluster_util::canLookupAtCoord(emptyStorage, desc, 0u, 0u, 0u),
               "canLookupAtCoord rejects empty storage");

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
               "coord lookup clamps OOB coords");
    expectTrue(clampedCoordLights.size() == 2u && clampedCoordLights[0] == 3u && clampedCoordLights[1] == 4u,
               "coord lookup at clamped last cluster copies assigned lights");
               "coord count at origin cluster");
               "coord count at empty cluster");
               "coord count clamps OOB tile/slice to last cluster");

    std::vector<fuse::u32> coordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, coordLights) == 2u,
               "coord lookup returns cluster count");
    expectTrue(coordLights.size() == 2u && coordLights[0] == 0u && coordLights[1] == 1u,

    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 99u, 99u, 99u,
                                                                         clampedCoordLights) == 2u,
               "coord lookup clamps OOB tile/slice to last cluster");
               "clamped coord lookup copies last cluster lights");

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
               "desc mismatch rejects population validation");
               "valid desc reports no population reject reason");

               "desc mismatch reports desc_mismatch population reason");
    expectTrue(std::strcmp(fuse::renderer::gridPopulationRejectReasonLabel(popReason), "desc_mismatch") == 0,
               "population reject label for desc mismatch");
}

void testClusterScreenLookupAndPopulationDescGuards() {
void testClusterRebuildPreflightGuards() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;
    const fuse::u32 clusterCount = desc.clusterCount();

    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGridForDesc(desc),
               "canRebuildForDesc accepts matching desc");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipRebuildLightGrid(desc, clusterCount),
               "shouldSkipRebuild false for matching cluster count");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipClusterRebuild(desc, clusterCount),
               "shouldSkipClusterRebuild false for matching cluster count");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipRebuildLightGrid(desc, clusterCount + 1u),
               "shouldSkipRebuild true for oversized cluster count");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterRebuild(desc, clusterCount + 1u),
               "shouldSkipClusterRebuild true for oversized cluster count");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipRebuildLightGrid(desc, 0u),
               "zero cluster count never skips rebuild");

    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };

    fuse::renderer::GridRebuildRejectReason rebuildReason = fuse::renderer::GridRebuildRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount, rebuildReason),
               "tryCanRebuild accepts matching cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::None,
               "matching cluster count reports no rebuild reject reason");
               "matching count reports no rebuild reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "none") == 0,
               "rebuild reject label for none");

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, 0u, rebuildReason),
               "tryCanRebuild accepts zero cluster count");
               "zero cluster count reports no rebuild reject reason");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, 0u),
               "shouldSkip does not skip zero cluster count rebuild");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount + 1u, rebuildReason),
               "tryCanRebuild rejects oversized cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::ClusterCountMismatch,
               "oversized cluster count reports cluster_count_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "cluster_count_mismatch") == 0,
               "rebuild reject label for cluster count mismatch");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, clusterCount + 1u),
               "shouldSkip skips mismatched cluster count rebuild");

    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::CountMismatch,
               "oversized count reports count_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "count_mismatch") == 0,
               "rebuild reject label for count mismatch");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, 4u, rebuildReason),
               "tryCanRebuild rejects non-zero count on empty desc");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::EmptyGrid,
               "empty desc reports empty_grid rebuild reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "empty_grid") == 0,
               "rebuild reject label for empty grid");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(zeroDesc, 4u),
               "shouldSkip skips rebuild on empty desc with non-zero count");
               "tryCanRebuild vacuously accepts zero cluster count");


    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, clusterCount, rebuildReason),
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::EmptyDesc,
               "empty desc reports empty_desc reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "empty_desc") == 0,
               "rebuild reject label for empty desc");
    fuse::renderer::GridRebuildRejectReason reason = fuse::renderer::GridRebuildRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount, reason),
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::None, "matching count reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(reason), "none") == 0,

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, 0u, reason),
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::None, "zero count reports no reject reason");
               "shouldSkip rebuild false for zero cluster count");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount + 1u, reason),
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::DescMismatch,
               "oversized count reports desc_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(reason), "desc_mismatch") == 0,
               "rebuild reject label for desc mismatch");
               "shouldSkip rebuild true for rejected non-zero count");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, 4u, reason),
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::EmptyGrid,
               "empty desc reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(reason), "empty_grid") == 0,

    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };

    fuse::renderer::ClusterGridSoA grid{};
    fuse::u32 dropped = 0u;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(grid, desc, perClusterLights, 2u,
                                                                                  dropped),
               "tryRebuild succeeds on valid desc");
    expectTrue(dropped == 0u, "tryRebuild reports zero dropped lights when within capacity");
    expectTrue(fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, desc),
               "hasAssignedLightsForDesc true after rebuild");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, desc) == 3u,
               "countNonEmptyClustersForDesc matches rebuilt grid");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, desc) == 1u,
               "countEmptyClustersForDesc matches rebuilt grid");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, mismatched) == 0u,
               "countNonEmptyClustersForDesc rejects desc mismatch");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, mismatched) == 0u,
               "countEmptyClustersForDesc rejects desc mismatch");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, mismatched),
               "hasAssignedLightsForDesc rejects desc mismatch");

    fuse::renderer::GridPopulationRejectReason offsetReason = fuse::renderer::GridPopulationRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsetsForDesc(grid, desc, offsetReason),
               "tryValidateContiguousOffsetsForDesc accepts rebuilt grid");
    expectTrue(offsetReason == fuse::renderer::GridPopulationRejectReason::None,
               "valid offsets report no reject reason");
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);
    const fuse::u32 preservedLightListSize = static_cast<fuse::u32>(grid.lightList.size());

    fuse::renderer::ClusterGridSoA guardedGrid = grid;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(
                   guardedGrid, desc, perClusterLights, 2u, dropped, rebuildReason),
               "tryRebuildForDesc succeeds on valid desc");
    expectTrue(dropped == 0u, "tryRebuildForDesc reports zero dropped lights within capacity");
               "successful guarded rebuild clears reject reason");

    fuse::renderer::ClusterGridSoA rejectedGrid = grid;
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(
                   rejectedGrid, desc, clusterCount + 1u, perClusterLights, 2u, dropped, rebuildReason),
               "tryRebuild rejects oversized cluster count without rebuilding");
    expectTrue(dropped == 0u, "rejected guarded rebuild zeroes dropped count");
               "oversized count reports count_mismatch on guarded rebuild");
    expectTrue(rejectedGrid.lightList.size() == preservedLightListSize,
               "rejected guarded rebuild leaves grid unchanged");

    fuse::renderer::ClusterGridSoA emptyRejectedGrid{};
                   emptyRejectedGrid, zeroDesc, clusterCount, perClusterLights, 2u, dropped, rebuildReason),
               "tryRebuild rejects non-zero count on empty desc without rebuilding");
               "empty desc guarded rebuild reports empty_desc reason");
    expectTrue(emptyRejectedGrid.grid.empty() && emptyRejectedGrid.lightList.empty(),
               "rejected empty-desc rebuild leaves storage empty");

    fuse::renderer::ClusterGridSoA clearedGrid{};
    clearedGrid.grid.resize(4u);
    clearedGrid.lightList = {0u, 1u, 2u};
                   clearedGrid, zeroDesc, perClusterLights, 2u, dropped, rebuildReason),
               "tryRebuildForDesc vacuously succeeds on empty desc");
    expectTrue(clearedGrid.grid.empty() && clearedGrid.lightList.empty(),
               "empty-desc rebuildForDesc clears storage");
}

void testClusterContiguousOffsetPreflightGuards() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 2;
    desc.tilesY = 2;
    desc.slicesZ = 1;
    const fuse::u32 clusterCount = desc.clusterCount();


    expectTrue(fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsets(grid, clusterCount, offsetReason),
               "tryValidateContiguousOffsets accepts rebuilt grid");
               "valid desc-scoped offsets report no reject reason");

    fuse::renderer::ClusterGridSoA undersized{};
    undersized.grid.resize(2u);
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsets(undersized, clusterCount,
                                                                                     offsetReason),
               "tryValidateContiguousOffsets rejects undersized grid");
    expectTrue(offsetReason == fuse::renderer::GridPopulationRejectReason::UndersizedGrid,
               "undersized grid reports undersized reason");

    fuse::renderer::ClusterGridSoA brokenOffsets{};
    brokenOffsets.grid.resize(clusterCount);
    brokenOffsets.grid[0] = {0u, 1u};
    brokenOffsets.grid[1] = {5u, 0u};
    brokenOffsets.lightList = {0u};
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsetsForDesc(brokenOffsets, desc,
               "tryValidateContiguousOffsetsForDesc rejects broken offsets");
    expectTrue(offsetReason == fuse::renderer::GridPopulationRejectReason::NonContiguousOffsets,
               "broken offsets report non_contiguous_offsets reason");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsetsForDesc(brokenOffsets, mismatched,
               "tryValidateContiguousOffsetsForDesc rejects desc mismatch");
    expectTrue(offsetReason == fuse::renderer::GridPopulationRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch reason on offset validation");

    fuse::u32 rejectedDropped = 99u;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(grid, zeroDesc, perClusterLights, 2u,
                                                                                  rejectedDropped),
               "tryRebuild vacuously succeeds on empty desc");
    expectTrue(rejectedDropped == 0u, "tryRebuild clears grid on empty desc");
    expectTrue(grid.grid.empty() && grid.lightList.empty(), "tryRebuild on empty desc clears storage");

    expectTrue(fuse::renderer::cluster_util::shouldSkipGridPopulationValidation(zeroDesc),
               "shouldSkipGridPopulationValidation true on empty desc");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipGridPopulationValidation(desc),
               "shouldSkipGridPopulationValidation false on non-empty desc");

void testClusterCoordLookupRejectReasons() {
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsets(brokenOffsets, clusterCount,
               "tryValidateContiguousOffsets rejects non-contiguous offsets");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryValidateContiguousOffsetsForDesc(emptyGrid, zeroDesc,
               "empty desc vacuously validates contiguous offsets");
               "empty desc reports no offset reject reason");

void testClusterCoordLookupRejectReasonAndScreenMapping() {

    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, clusterCount + 1u),
               "shouldSkip rebuild on count mismatch");
               "shouldSkip rebuild on empty desc with non-zero count");

    const fuse::u32 preservedListSize = static_cast<fuse::u32>(grid.lightList.size());
    const fuse::u32 preservedGridCount = static_cast<fuse::u32>(grid.grid.size());

    fuse::u32 rejectedDropped = 0u;
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(grid, desc, clusterCount + 1u,
                                                                            perClusterLights, 2u, rejectedDropped),
               "tryRebuild rejects count mismatch");
    expectTrue(rejectedDropped == 0u, "rejected tryRebuild zeroes dropped count");
    expectTrue(grid.lightList.size() == preservedListSize, "rejected tryRebuild preserves light list");
    expectTrue(grid.grid.size() == preservedGridCount, "rejected tryRebuild preserves grid entries");

    fuse::u32 acceptedDropped = 0u;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(grid, desc, clusterCount, perClusterLights,
                                                                           2u, acceptedDropped),
               "tryRebuild accepts matching count");
    expectTrue(acceptedDropped == 0u, "tryRebuild reports dropped overflow count");
    expectTrue(grid.lightList.size() == 5u, "tryRebuild repacks flat light list");

    fuse::u32 forDescDropped = 0u;
                                                                                  forDescDropped),
               "tryRebuildForDesc accepts matching desc");
    expectTrue(forDescDropped == 0u, "tryRebuildForDesc reports dropped overflow count");

    fuse::u32 emptyDescDropped = 0u;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(emptyGrid, zeroDesc, perClusterLights,
                                                                                  2u, emptyDescDropped),
    expectTrue(emptyDescDropped == 0u, "empty desc rebuild drops nothing");
    expectTrue(emptyGrid.grid.empty() && emptyGrid.lightList.empty(),
               "empty desc rebuild clears storage");

                                                                                  dropped, reason),
               "tryRebuildForDesc succeeds on matching desc");
    expectTrue(dropped == 0u, "tryRebuildForDesc reports zero dropped lights");
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::None,
               "successful rebuild clears reject reason");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulationForDesc(grid, desc),
               "tryRebuildForDesc produces valid population");

    fuse::renderer::ClusterGridSoA rejectedGrid{};
    rejectedGrid.grid.resize(clusterCount);
    rejectedGrid.lightList = {0u, 1u, 2u};
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(rejectedGrid, desc, clusterCount + 1u,
                                                                          perClusterLights, 2u, rejectedDropped,
                                                                          reason),
               "tryRebuild rejects oversized cluster count");
    expectTrue(rejectedDropped == 0u, "rejected rebuild leaves dropped count at zero");
    expectTrue(rejectedGrid.lightList.size() == 3u, "rejected rebuild preserves prior light list");
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::DescMismatch,
               "rejected rebuild preserves desc_mismatch reason");

    fuse::renderer::ClusterGridSoA emptyDescGrid{};
    emptyDescGrid.grid.resize(4u);
    emptyDescGrid.lightList = {0u, 1u};
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(emptyDescGrid, zeroDesc,
                                                                                  perClusterLights, 2u,
                                                                                  rejectedDropped, reason),
    expectTrue(rejectedDropped == 0u, "empty desc rebuild drops nothing");
    expectTrue(emptyDescGrid.grid.empty() && emptyDescGrid.lightList.empty(),
               "empty desc rebuild reports no reject reason");

    fuse::renderer::ClusterGridSoA explicitCountGrid{};
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(explicitCountGrid, zeroDesc, 4u,
               "tryRebuild rejects explicit non-zero count on empty desc");
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::EmptyGrid,
               "explicit count on empty desc reports empty_grid reason");

void testClusterScreenDepthMappingGuards() {
    desc.tilesX = 4;
    desc.slicesZ = 3;

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::u32 clusterIndex = 0u;
    expectTrue(fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, desc, camera, clusterIndex),
               "tryMap succeeds for in-range screen depth");
    expectTrue(clusterIndex < desc.clusterCount(), "tryMap cluster index in bounds");

    fuse::u32 rejectedIndex = 0u;
    expectTrue(!fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 0.01f, desc, camera, rejectedIndex),
               "tryMap rejects depth below near plane");
    expectTrue(fuse::renderer::ClusterGridLayout::shouldSkipScreenDepthMapping(desc, camera, 0.01f),
               "shouldSkip true for depth below near plane");
    expectTrue(fuse::renderer::ClusterGridLayout::shouldSkipScreenDepthMapping(desc, camera, 200.f),
               "shouldSkip true for depth above far plane");
    expectTrue(!fuse::renderer::ClusterGridLayout::shouldSkipScreenDepthMapping(desc, camera, 10.f),
               "shouldSkip false for in-range depth");

    expectTrue(fuse::renderer::ClusterGridLayout::shouldSkipScreenDepthMapping(zeroDesc, camera, 10.f),
               "shouldSkip true for empty grid");
                   0.5f, 0.5f, 10.f, zeroDesc, camera, rejectedIndex),
               "tryMap rejects empty grid");

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

    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGridForDesc(desc),
               "canRebuildForDesc accepts matching desc");
    expectTrue(fuse::renderer::cluster_util::canLookupAtCoord(grid, desc, 0u, 0u, 0u),
               "canLookupAtCoord accepts accessible grid");
    expectTrue(fuse::renderer::cluster_util::canLookupAtCoord(grid, desc, 99u, 99u, 99u),
               "canLookupAtCoord accepts OOB coords that will be clamped");

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

    fuse::u32 coordCount = 0u;
    std::vector<fuse::u32> coordLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, coordLights,
                                                                           coordCount, lookupReason),
               "tryCoordLookup with reason succeeds on accessible grid");
    expectTrue(coordCount == 2u, "tryCoordLookup with reason reports cluster light count");
               "successful coord lookup clears reject reason");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 1u, 1u, 0u) == 2u,
               "count at last cluster coord");

    std::vector<fuse::u32> originLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, originLights) == 2u,
               "lookup at origin coord returns cluster count");
    expectTrue(originLights.size() == 2u && originLights[0] == 0u && originLights[1] == 1u,
               "lookup at origin coord copies assigned lights");

    std::vector<fuse::u32> clampedLastLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 99u, 99u, 99u, clampedLastLights) ==
               "lookup at OOB coord clamps to last cluster");
    expectTrue(clampedLastLights.size() == 2u && clampedLastLights[0] == 3u && clampedLastLights[1] == 4u,
               "lookup at clamped last coord copies assigned lights");

    fuse::u32 tryCount = 0u;
    std::vector<fuse::u32> tryLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryLights, tryCount),
               "tryLookup at coord succeeds on valid grid");
    expectTrue(tryCount == 2u, "tryLookup at coord reports cluster light count");

    fuse::u32 rejectedCount = 0u;
    std::vector<fuse::u32> rejectedLights;
                                                                            rejectedLights, rejectedCount),
               "tryLookup at coord rejects empty storage");
    expectTrue(rejectedCount == 0u, "tryLookup at coord zeroes count on guard failure");

void testClusterLookupFromScreenGuards() {
    desc.tilesX = 4;
    desc.slicesZ = 3;

    std::vector<std::vector<fuse::u32>> perClusterLights(clusterCount);
    perClusterLights[0] = {7u, 8u};
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 4u);
    fuse::renderer::ClusterLookupRejectReason reason = fuse::renderer::ClusterLookupRejectReason::None;
    fuse::u32 tryCoordCount = 0u;
    std::vector<fuse::u32> tryCoordLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryCoordLights,
                                                                           tryCoordCount, reason),
    expectTrue(tryCoordCount == 2u, "tryCoordLookup with reason reports cluster light count");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::None,


    fuse::u32 rejectedCoordCount = 0u;
    std::vector<fuse::u32> rejectedCoordLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(emptyGrid, desc, 0u, 0u, 0u,
                                                                            rejectedCoordLights, rejectedCoordCount,
                                                                            reason),
               "tryCoordLookup with reason rejects empty storage");
    expectTrue(rejectedCoordCount == 0u, "rejected coord lookup zeroes count");
    expectTrue(rejectedCoordLights.empty(), "rejected coord lookup clears output");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "rejected coord lookup preserves empty_storage reason");

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::u32 screenCount = 0u;
    std::vector<fuse::u32> screenLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsFromScreen(grid, desc, camera, 0.25f, 0.25f, 10.f,
                                                                              screenLights, screenCount),
               "screen lookup succeeds for in-bounds depth");
    expectTrue(screenCount == 2u, "screen lookup reports cluster light count at mapped cell");
    expectTrue(screenLights.size() == 2u && screenLights[0] == 0u && screenLights[1] == 1u,
               "screen lookup copies assigned lights at mapped cell");

    fuse::u32 belowNearCount = 0u;
    std::vector<fuse::u32> belowNearLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsFromScreen(grid, desc, camera, 0.5f, 0.5f, 0.01f,
                                                                               belowNearLights, belowNearCount,
                                                                               lookupReason),
               "screen lookup rejects depth below near plane");
    expectTrue(belowNearCount == 0u, "screen lookup zeroes count on mapping failure");
    expectTrue(belowNearLights.empty(), "screen lookup clears output on mapping failure");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::ScreenMappingFailed,
               "mapping failure reports screen_mapping_failed reason");
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(lookupReason), "screen_mapping_failed") == 0,
               "lookup reject label for screen mapping failure");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsFromScreen(grid, zeroDesc, camera, 0.5f, 0.5f,
                                                                               10.f, screenLights, screenCount,
               "screen lookup rejects empty grid desc");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::EmptyGrid,
               "empty grid reports empty_grid reason for screen lookup");

    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, desc) == 3u,
               "countNonEmptyClustersForDesc matches rebuilt grid");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, desc) == 1u,
               "countEmptyClustersForDesc matches rebuilt grid");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCountsForDesc(grid, desc),
               "validatePopulationCountsForDesc accepts rebuilt grid");
    expectTrue(!fuse::renderer::cluster_util::isPopulationFullyEmpty(grid, desc),
               "rebuilt grid is not fully empty");

    fuse::renderer::ClusterGridSoA emptyPopulation{};
    emptyPopulation.grid.resize(clusterCount);
    expectTrue(fuse::renderer::cluster_util::isPopulationFullyEmpty(emptyPopulation, desc),
               "allocated but unassigned grid is fully empty");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(emptyPopulation, desc) == 0u,
               "countNonEmptyClustersForDesc zero for fully empty population");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(emptyPopulation, desc) == clusterCount,
               "countEmptyClustersForDesc matches cluster count for fully empty population");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCountsForDesc(emptyPopulation, desc),
               "validatePopulationCountsForDesc accepts fully empty population");

    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, mismatched) == 0u,
               "countNonEmptyClustersForDesc rejects desc mismatch");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, mismatched) == 1u,
               "countEmptyClustersForDesc returns mismatched cluster count");
    expectTrue(!fuse::renderer::cluster_util::validatePopulationCountsForDesc(grid, mismatched),
               "validatePopulationCountsForDesc rejects desc mismatch");
    expectTrue(!fuse::renderer::cluster_util::isPopulationFullyEmpty(grid, mismatched),
               "desc mismatch is not treated as fully empty population");
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsFromScreen(
                   grid, desc, camera, 0.f, 0.f, 1.f, screenLights, screenCount),
               "screen lookup succeeds for valid screen/depth");
    expectTrue(screenCount == 2u, "screen lookup reports cluster light count");
    expectTrue(screenLights.size() == 2u && screenLights[0] == 7u && screenLights[1] == 8u,
               "screen lookup copies origin cluster lights");

    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsFromScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, belowNearLights, belowNearCount),
    expectTrue(belowNearCount == 0u, "screen lookup zeroes count when mapping fails");

    fuse::u32 zeroGridCount = 0u;
    std::vector<fuse::u32> zeroGridLights;
                   grid, zeroDesc, camera, 0.5f, 0.5f, 10.f, zeroGridLights, zeroGridCount),

void testClusterRebuildAndPopulationPreflightGuards() {


    expectTrue(fuse::renderer::cluster_util::hasAssignedLights(grid, clusterCount),
               "rebuilt grid has assigned lights");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(fuse::renderer::ClusterGridSoA{}, clusterCount),
               "empty storage has no assigned lights");

               "tryLookup at coord succeeds on accessible grid");
    expectTrue(tryCoordCount == 2u, "tryLookup at coord reports cluster light count");

                                                                           tryCoordCount, lookupReason),
               "tryLookup at coord with reason succeeds on accessible grid");

    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(emptyGrid, desc, 0u, 0u, 0u,
                                                                        rejectedCoordLights) == 0u,
               "coord lookup rejects inaccessible grid");
    expectTrue(rejectedCoordLights.empty(), "coord lookup clears output on guard failure");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(emptyGrid, desc, 0u, 0u, 0u) == 0u,
               "coord count zero on inaccessible grid");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(emptyGrid, clusterCount),

    expectTrue(rejectedCoordCount == 0u, "tryLookup at coord zeroes count on failure");
               "failed coord lookup preserves reject reason");

    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, clusterCount),
               "rebuild allowed when cluster count matches desc");
    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, 0u),
               "zero cluster rebuild is always allowed");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, clusterCount + 1u),
               "rebuild rejected when cluster count mismatches desc");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(zeroDesc, clusterCount),
               "rebuild rejected when desc is empty but cluster count is non-zero");

    fuse::renderer::GridPopulationRejectReason popReason = fuse::renderer::GridPopulationRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, zeroDesc, popReason),
               "tryValidateGridPopulationForDesc vacuously accepts empty desc");
    expectTrue(popReason == fuse::renderer::GridPopulationRejectReason::None,
               "empty desc reports no population reject reason");
    expectTrue(!fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, zeroDesc, popReason),
               "population validation rejects non-empty count on empty desc");
    expectTrue(popReason == fuse::renderer::GridPopulationRejectReason::EmptyGrid,
               "empty desc reports empty_grid population reason");
    expectTrue(std::strcmp(fuse::renderer::gridPopulationRejectReasonLabel(popReason), "empty_grid") == 0,
               "population reject label for empty grid");
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryLights, tryCount,
    expectTrue(tryCount == 2u, "tryCoordLookup with reason reports cluster light count");

                                                                            rejectedLights, rejectedCount, reason),
    expectTrue(rejectedCount == 0u, "tryCoordLookup with reason zeroes count on failure");
    expectTrue(rejectedLights.empty(), "tryCoordLookup with reason clears output on failure");

    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, zeroDesc, 0u, 0u, 0u, rejectedLights,
                                                                            rejectedCount, reason),
               "tryCoordLookup with reason rejects empty grid desc");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyGrid,
               "empty grid desc reports empty_grid reason");
    fuse::u32 mappedIndex = 0u;
    expectTrue(fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, desc, camera, mappedIndex),
               "tryMapScreenDepth accepts valid screen depth");
    expectTrue(mappedIndex < clusterCount, "tryMapScreenDepth maps to valid cluster index");

    fuse::u32 rejectedIndex = 99u;
    expectTrue(!fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, rejectedIndex),
               "tryMapScreenDepth rejects empty grid desc");
    expectTrue(rejectedIndex == 99u, "tryMapScreenDepth does not write index on empty grid");
    expectTrue(rejectedCoordCount == 0u, "tryCoordLookup with reason zeroes count on failure");
    expectTrue(rejectedCoordLights.empty(), "tryCoordLookup with reason clears output on failure");
               "failed coord lookup preserves empty_storage reason");
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(reason), "empty_storage") == 0,
               "coord lookup reject label for empty storage");

    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, zeroDesc, 0u, 0u, 0u,
               "empty grid desc reports empty_grid coord lookup reason");

    expectTrue(rejectedCount == 0u, "rejected coord lookup zeroes count");
    expectTrue(rejectedLights.empty(), "rejected coord lookup clears output");
               "empty storage coord lookup reports empty_storage reason");

               "empty grid coord lookup reports empty_grid reason");
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

void testClusterGridAccessibleGuard() {
void testClusterLightGridAccessAndCoordLookupGuards() {
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

    expectTrue(fuse::renderer::cluster_util::isClusterGridAccessible(grid, desc),
               "accessible when desc matches storage");
    expectTrue(fuse::renderer::cluster_util::canLookupAtIndex(grid, desc, 0u),
               "canLookupAtIndex agrees with isClusterGridAccessible");
    expectTrue(fuse::renderer::cluster_util::canLookupAtCoord(grid, desc, 0u, 0u, 0u),
               "canLookupAtCoord accepts matching grid");
    expectTrue(fuse::renderer::cluster_util::isLightGridAccessible(grid, desc),
               "rebuilt grid is accessible");
    expectTrue(fuse::renderer::cluster_util::hasAssignedLights(grid, desc),
               "rebuilt grid has assigned lights");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipClusterLightLookup(grid, desc),
               "populated grid does not skip lookup");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::isLightGridAccessible(emptyGrid, desc),
               "empty storage is not accessible");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(emptyGrid, desc),
               "empty storage has no assigned lights");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLightLookup(emptyGrid, desc),
               "empty storage skips cluster light lookup");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::cluster_util::isLightGridAccessible(grid, zeroDesc),
               "zero-dimension desc is not accessible");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLightLookup(grid, zeroDesc),
               "zero-dimension desc skips cluster light lookup");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(!fuse::renderer::cluster_util::isClusterGridAccessible(grid, mismatched),
               "inaccessible when desc mismatches storage");
    expectTrue(!fuse::renderer::cluster_util::canLookupAtCoord(grid, mismatched, 0u, 0u, 0u),
               "canLookupAtCoord rejects desc mismatch");

    expectTrue(!fuse::renderer::cluster_util::isClusterGridAccessible(grid, zeroDesc),
               "inaccessible when desc is empty grid");
    expectTrue(fuse::renderer::cluster_util::isClusterGridAccessible(grid, zeroDesc) ==
                   fuse::renderer::cluster_util::canLookupAtIndex(grid, zeroDesc, 0u),
               "canLookupAtIndex matches accessibility on empty desc");

    expectTrue(!fuse::renderer::cluster_util::isClusterGridAccessible(emptyGrid, desc),
               "inaccessible when storage is empty");
}

void testClusterLookupAtCoordGuards() {


    expectTrue(!fuse::renderer::cluster_util::isLightGridAccessible(grid, mismatched),
               "desc mismatch is not accessible");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLightLookup(grid, mismatched),
               "desc mismatch skips cluster light lookup");

    fuse::renderer::ClusterGridSoA allocatedEmpty{};
    allocatedEmpty.allocate(desc);
    expectTrue(fuse::renderer::cluster_util::isLightGridAccessible(allocatedEmpty, desc),
               "allocated empty grid is accessible");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(allocatedEmpty, desc),
               "allocated empty grid has no assigned lights");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLightLookup(allocatedEmpty, desc),
               "allocated empty grid skips lookup");

    expectTrue(!fuse::renderer::cluster_util::canLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u),
               "canLookupAtCoord rejects empty storage");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 0u, 0u, 0u) == 2u,
               "count at origin coord");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 1u, 0u, 0u) == 0u,
               "count at empty cluster coord");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 1u, 1u, 0u) == 2u,
               "count at last cluster coord");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 99u, 99u, 99u) == 2u,
               "count clamps OOB coords to last cluster");

    std::vector<fuse::u32> originLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, originLights) == 2u,
               "lookup at origin coord returns cluster count");
    expectTrue(originLights.size() == 2u && originLights[0] == 0u && originLights[1] == 1u,
               "lookup at origin coord copies assigned lights");

    std::vector<fuse::u32> clampedLastLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 99u, 99u, 99u, clampedLastLights) ==
                   2u,
               "lookup at OOB coord clamps to last cluster");
    expectTrue(clampedLastLights.size() == 2u && clampedLastLights[0] == 3u && clampedLastLights[1] == 4u,
               "lookup at clamped last coord copies assigned lights");

    fuse::u32 tryCount = 0u;
    std::vector<fuse::u32> tryLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryLights, tryCount),
               "tryLookup at coord succeeds on valid grid");
    expectTrue(tryCount == 2u, "tryLookup at coord reports cluster light count");

    fuse::u32 rejectedCount = 0u;
    std::vector<fuse::u32> rejectedLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(emptyGrid, desc, 0u, 0u, 0u,
                                                                            rejectedLights, rejectedCount),
               "tryLookup at coord rejects empty storage");
    expectTrue(rejectedCount == 0u, "tryLookup at coord zeroes count on guard failure");

void testClusterLookupFromScreenGuards() {
void testClusterScreenMappingRejectReasons() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::ClusterGridSoA grid{};
    const fuse::u32 clusterCount = desc.clusterCount();
    std::vector<std::vector<fuse::u32>> perClusterLights(clusterCount);
    perClusterLights[0] = {7u, 8u};
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 4u);

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    fuse::u32 screenCount = 0u;
    std::vector<fuse::u32> screenLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsFromScreen(
                   grid, desc, camera, 0.f, 0.f, 1.f, screenLights, screenCount),
               "screen lookup succeeds for valid screen/depth");
    expectTrue(screenCount == 2u, "screen lookup reports cluster light count");
    expectTrue(screenLights.size() == 2u && screenLights[0] == 7u && screenLights[1] == 8u,
               "screen lookup copies origin cluster lights");

    fuse::u32 belowNearCount = 0u;
    std::vector<fuse::u32> belowNearLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsFromScreen(
                   grid, desc, camera, 0.5f, 0.5f, 0.01f, belowNearLights, belowNearCount),
               "screen lookup rejects depth below near plane");
    expectTrue(belowNearCount == 0u, "screen lookup zeroes count when mapping fails");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    fuse::u32 zeroGridCount = 0u;
    std::vector<fuse::u32> zeroGridLights;
                   grid, zeroDesc, camera, 0.5f, 0.5f, 10.f, zeroGridLights, zeroGridCount),
               "screen lookup rejects empty grid desc");

void testTryValidateGridPopulationForDesc() {
    desc.tilesX = 2;
    desc.slicesZ = 1;

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

    fuse::u32 emptyIndex = 99u;
    expectTrue(!fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, emptyIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects empty grid");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::EmptyGrid,
               "empty grid reports empty_grid reason");
    expectTrue(emptyIndex == 0u, "empty grid mapping zeroes output index");
    expectTrue(std::strcmp(fuse::renderer::clusterScreenMappingRejectReasonLabel(mapReason), "empty_grid") == 0,
               "screen mapping reject label for empty grid");

    fuse::u32 depthIndex = 0u;
                   0.5f, 0.5f, 0.01f, desc, camera, depthIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects depth below near plane");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::DepthOutOfRange,
               "depth below near plane reports depth_out_of_range reason");
    expectTrue(std::strcmp(fuse::renderer::clusterScreenMappingRejectReasonLabel(mapReason), "depth_out_of_range") ==
                   0,
               "screen mapping reject label for depth out of range");

    fuse::renderer::ClusterCameraDesc invalidCamera{};
    invalidCamera.nearPlane = 0.f;
    invalidCamera.farPlane = 100.f;
    fuse::u32 invalidIndex = 0u;
                   0.5f, 0.5f, 10.f, desc, invalidCamera, invalidIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects invalid camera");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera reason");
    expectTrue(std::strcmp(fuse::renderer::clusterScreenMappingRejectReasonLabel(mapReason), "invalid_camera") == 0,
               "screen mapping reject label for invalid camera");
}

void testGridRebuildRejectReasons() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesY = 2;
    const fuse::u32 clusterCount = desc.clusterCount();

    fuse::renderer::GridRebuildRejectReason rebuildReason = fuse::renderer::GridRebuildRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount, rebuildReason),
               "tryCanRebuild accepts matching cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::None,
               "matching count reports no rebuild reject reason");
    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, clusterCount),
               "canRebuild delegates to tryCanRebuild");

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, 0u, rebuildReason),
               "tryCanRebuild vacuously accepts zero cluster count");
               "zero cluster count reports no rebuild reject reason");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount + 1u, rebuildReason),
               "tryCanRebuild rejects oversized cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::CountMismatch,
               "oversized count reports count_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "count_mismatch") == 0,
               "rebuild reject label for count mismatch");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, 4u, rebuildReason),
               "tryCanRebuild rejects non-zero count on empty desc");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::EmptyGrid,
               "empty desc with non-zero count reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "empty_grid") == 0,
               "rebuild reject label for empty grid");

void testClusterCoordLookupRejectReasonsAndSkipGrid() {

    fuse::renderer::ClusterGridSoA grid{};
    const std::vector<std::vector<fuse::u32>> perClusterLights = {
        {0u, 1u},
        {},
        {2u},
        {3u, 4u},
    };
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights, 2u);

    fuse::renderer::GridPopulationRejectReason reason = fuse::renderer::GridPopulationRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, desc, reason),
               "tryValidateForDesc accepts rebuilt grid");
    expectTrue(reason == fuse::renderer::GridPopulationRejectReason::None,
               "valid grid reports no reject reason via desc helper");

    fuse::renderer::ClusterDesc mismatchedDesc{};
    mismatchedDesc.tilesX = 4;
    mismatchedDesc.tilesY = 2;
    mismatchedDesc.slicesZ = 4;
    expectTrue(!fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, mismatchedDesc, reason),
               "tryValidateForDesc rejects desc mismatch");
    expectTrue(reason == fuse::renderer::GridPopulationRejectReason::UndersizedGrid,
               "larger desc reports undersized grid reason");

    expectTrue(fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(grid, zeroDesc, reason),
               "tryValidateForDesc vacuously accepts empty desc");
               "empty desc reports no reject reason");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, mismatched, 0u, 0u, 0u) == 0u,
               "count at coord rejects desc mismatch");

    std::vector<fuse::u32> originCoordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, originCoordLights) == 2u,
    expectTrue(originCoordLights.size() == 2u && originCoordLights[0] == 0u && originCoordLights[1] == 1u,

    std::vector<fuse::u32> lastCoordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 99u, 99u, 99u, lastCoordLights) == 2u,
    expectTrue(lastCoordLights.size() == 2u && lastCoordLights[0] == 3u && lastCoordLights[1] == 4u,

    fuse::u32 tryCoordCount = 0u;
    std::vector<fuse::u32> tryCoordLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryCoordLights,
                                                                            tryCoordCount),
    expectTrue(tryCoordCount == 2u, "tryLookup at coord reports cluster light count");
    expectTrue(tryCoordLights.size() == 2u && tryCoordLights[0] == 0u && tryCoordLights[1] == 1u,
               "tryLookup at coord copies assigned lights");

    fuse::u32 rejectedCoordCount = 0u;
    std::vector<fuse::u32> rejectedCoordLights;
                                                                             rejectedCoordLights, rejectedCoordCount),
    expectTrue(rejectedCoordCount == 0u, "tryLookup at coord zeroes count on guard failure");
    expectTrue(rejectedCoordLights.empty(), "tryLookup at coord clears output on guard failure");

               "tryValidateGridPopulationForDesc accepts rebuilt grid");

    fuse::renderer::ClusterGridSoA undersized{};
    undersized.grid.resize(2u);
    expectTrue(!fuse::renderer::cluster_util::tryValidateGridPopulationForDesc(undersized, desc, reason),
               "tryValidateGridPopulationForDesc rejects undersized grid");
               "undersized grid reports undersized reason via desc helper");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipClusterGrid(desc),
               "non-empty desc does not skip cluster grid");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterGrid(zeroDesc),
               "empty desc skips cluster grid populate");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterGrid(zeroDesc) ==
                   fuse::renderer::cluster_util::shouldSkipClusterCull(zeroDesc),
               "shouldSkipClusterGrid matches shouldSkipClusterCull on empty desc");
    expectTrue(fuse::renderer::cluster_util::canLookupAtCoord(grid, desc, 0u, 0u, 0u),
               "coord lookup preflight accepts accessible grid");
    expectTrue(fuse::renderer::cluster_util::canLookupAtCoord(grid, desc, 99u, 99u, 99u),
               "coord lookup preflight ignores coords when storage matches desc");

    fuse::renderer::ClusterLookupRejectReason coordReason = fuse::renderer::ClusterLookupRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(
                   grid, desc, 0u, 0u, 0u, tryCoordLights, tryCoordCount, coordReason),
               "tryCoordLookup with reason succeeds on accessible grid");
    expectTrue(tryCoordCount == 2u, "tryCoordLookup with reason reports cluster light count");
    expectTrue(coordReason == fuse::renderer::ClusterLookupRejectReason::None,
               "successful coord lookup clears reject reason");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(
                   emptyGrid, desc, 0u, 0u, 0u, rejectedCoordLights, rejectedCoordCount, coordReason),
               "tryCoordLookup with reason rejects empty storage");
    expectTrue(rejectedCoordCount == 0u, "tryCoordLookup with reason zeroes count on failure");
    expectTrue(rejectedCoordLights.empty(), "tryCoordLookup with reason clears output on failure");
    expectTrue(coordReason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "failed coord lookup preserves reject reason");
    expectTrue(!fuse::renderer::cluster_util::canLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u),
               "coord lookup preflight rejects empty storage");
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

void testClusterLookupAtCoordGuards() {
void testClusterRebuildPreflightGuards() {
void testClusterLookupAtCoordRejectReasons() {
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

    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 0u, 0u, 0u) == 2u,
               "count at origin coord");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 1u, 0u, 0u) == 0u,
               "count at empty cluster coord");
    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtCoord(grid, desc, 99u, 99u, 99u) == 2u,
               "count clamps OOB coords to last cluster");

    fuse::u32 tryCount = 0u;
    std::vector<fuse::u32> tryLights;
    fuse::renderer::ClusterLookupRejectReason reason = fuse::renderer::ClusterLookupRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::tryClusterLightCountAtIndex(grid, desc, 0u, tryCount, reason),
               "tryClusterLightCountAtIndex succeeds on accessible grid");
    expectTrue(tryCount == 2u, "tryClusterLightCountAtIndex reports cluster light count");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::None,
               "tryClusterLightCountAtIndex clears reject reason on success");

    fuse::u32 rejectedCount = 0u;
    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::tryClusterLightCountAtIndex(emptyGrid, desc, 0u, rejectedCount, reason),
               "tryClusterLightCountAtIndex rejects empty storage");
    expectTrue(rejectedCount == 0u, "tryClusterLightCountAtIndex zeroes count on failure");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "tryClusterLightCountAtIndex reports empty_storage reason");

    std::vector<fuse::u32> originLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, originLights) == 2u,
               "lookup at origin coord returns cluster count");
    expectTrue(originLights.size() == 2u && originLights[0] == 0u && originLights[1] == 1u,
               "lookup at origin coord copies assigned lights");

    std::vector<fuse::u32> clampedCoordLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtCoord(grid, desc, 99u, 99u, 99u,
                                                                          clampedCoordLights) == 2u,
               "lookup at OOB coord clamps to last cluster");
    expectTrue(clampedCoordLights.size() == 2u && clampedCoordLights[0] == 3u && clampedCoordLights[1] == 4u,
               "lookup at clamped coord copies assigned lights");

    fuse::u32 coordTryCount = 0u;
    std::vector<fuse::u32> coordTryLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, coordTryLights,
                                                                           coordTryCount),
               "tryLookupClusterLightsAtCoord succeeds on accessible grid");
    expectTrue(coordTryCount == 2u, "tryLookupClusterLightsAtCoord reports cluster light count");
    expectTrue(coordTryLights.size() == 2u && coordTryLights[0] == 0u && coordTryLights[1] == 1u,
               "tryLookupClusterLightsAtCoord copies assigned lights");

    fuse::u32 coordRejectedCount = 0u;
    std::vector<fuse::u32> coordRejectedLights;
    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, zeroDesc, 0u, 0u, 0u,
                                                                            coordRejectedLights, coordRejectedCount,
                                                                            reason),
               "tryLookupClusterLightsAtCoord rejects empty grid desc");
    expectTrue(coordRejectedCount == 0u, "tryLookupClusterLightsAtCoord zeroes count on failure");
    expectTrue(coordRejectedLights.empty(), "tryLookupClusterLightsAtCoord clears output on failure");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::EmptyGrid,
               "tryLookupClusterLightsAtCoord reports empty_grid reason");
}

void testClusterAssignedLightingGuards() {


    expectTrue(!fuse::renderer::ClusterGridLayout::shouldSkipClusterGrid(desc),
               "shouldSkipClusterGrid false on non-empty desc");

    expectTrue(fuse::renderer::ClusterGridLayout::shouldSkipClusterGrid(zeroDesc),
               "shouldSkipClusterGrid true on empty desc");
    expectTrue(fuse::renderer::ClusterGridLayout::shouldSkipClusterGrid(zeroDesc) ==
                   fuse::renderer::ClusterGridLayout::isEmptyGrid(zeroDesc),
               "shouldSkipClusterGrid agrees with isEmptyGrid");

    expectTrue(fuse::renderer::cluster_util::hasAssignedLights(grid, desc),
               "hasAssignedLights true when grid has lights");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipClusterLighting(grid, desc),
               "cluster lighting not skipped when grid has assigned lights");

    fuse::renderer::ClusterGridSoA emptyAssignedGrid{};
    emptyAssignedGrid.allocate(desc);
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(emptyAssignedGrid, clusterCount, {}, 2u);
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(emptyAssignedGrid, desc),
               "hasAssignedLights false when all clusters empty");
    expectTrue(fuse::renderer::cluster_util::isGridPopulationEmpty(emptyAssignedGrid, clusterCount),
               "isGridPopulationEmpty true when all clusters empty");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLighting(emptyAssignedGrid, desc),
               "cluster lighting skipped when no lights assigned");

    fuse::renderer::ClusterGridSoA emptyStorage{};
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLights(emptyStorage, desc),
               "hasAssignedLights false on empty storage");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterLighting(emptyStorage, desc),
               "cluster lighting skipped on empty storage");

void testClusterDescScopedPopulationCounts() {

    fuse::renderer::GridRebuildRejectReason reason = fuse::renderer::GridRebuildRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount, reason),
               "tryCanRebuild accepts matching cluster count");
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::None, "matching count reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(reason), "none") == 0,
               "rebuild reject label for none");

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, 0u, reason),
               "tryCanRebuild vacuously accepts zero cluster count");
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::None,
               "zero cluster count reports no reject reason");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, clusterCount),
               "matching count does not skip rebuild");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, 0u),
               "zero cluster count does not skip rebuild");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount + 1u, reason),
               "tryCanRebuild rejects oversized cluster count");
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::ClusterCountMismatch,
               "oversized count reports cluster_count_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(reason), "cluster_count_mismatch") == 0,
               "rebuild reject label for cluster count mismatch");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, clusterCount + 1u),
               "oversized count skips rebuild");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, clusterCount, reason),
               "tryCanRebuild rejects non-zero count on empty desc");
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::EmptyGrid,
               "empty desc reports empty_grid rebuild reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(reason), "empty_grid") == 0,
               "rebuild reject label for empty grid");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(zeroDesc, clusterCount),
               "non-zero count on empty desc skips rebuild");
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, 0u, reason),
               "tryCanRebuild vacuously accepts zero count on empty desc");

void testClusterDescScopedPopulationGuards() {
    expectTrue(fuse::renderer::ClusterLightGridLayout::canRebuildLightGrid(desc, clusterCount),
               "canRebuild accepts matching cluster count");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipClusterRebuild(desc, clusterCount),

    fuse::renderer::ClusterRebuildRejectReason rebuildReason = fuse::renderer::ClusterRebuildRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount, rebuildReason),
    expectTrue(rebuildReason == fuse::renderer::ClusterRebuildRejectReason::None,
               "matching count reports no rebuild reject reason");
    expectTrue(std::strcmp(fuse::renderer::clusterRebuildRejectReasonLabel(rebuildReason), "none") == 0,

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, 0u, rebuildReason),
               "zero cluster count reports no rebuild reject reason");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipClusterRebuild(desc, 0u),

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount + 1u, rebuildReason),
    expectTrue(rebuildReason == fuse::renderer::ClusterRebuildRejectReason::CountMismatch,
               "oversized count reports count_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::clusterRebuildRejectReasonLabel(rebuildReason), "count_mismatch") == 0,
               "rebuild reject label for count mismatch");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipClusterRebuild(desc, clusterCount + 1u),
               "count mismatch skips rebuild");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, 4u, rebuildReason),
    expectTrue(rebuildReason == fuse::renderer::ClusterRebuildRejectReason::EmptyGrid,
    expectTrue(std::strcmp(fuse::renderer::clusterRebuildRejectReasonLabel(rebuildReason), "empty_grid") == 0,
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipClusterRebuild(zeroDesc, 4u),
               "empty desc with non-zero count skips rebuild");

void testClusterDirectLookupGuards() {
    const fuse::u32 clusterCount = 3u;
        {0u, 2u},
        {1u},
    fuse::renderer::ClusterLightGridLayout::rebuildLightGrid(grid, clusterCount, perClusterLights);

    expectTrue(fuse::renderer::cluster_util::canLookupCluster(grid, 0u),
               "canLookupCluster accepts in-range index");
    expectTrue(!fuse::renderer::cluster_util::canLookupCluster(grid, 99u),
               "canLookupCluster rejects OOB index");

    expectTrue(fuse::renderer::cluster_util::tryCanLookupCluster(grid, 0u, reason),
               "tryCanLookupCluster accepts in-range index");
               "in-range cluster lookup reports no reject reason");

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupCluster(grid, 99u, reason),
               "tryCanLookupCluster rejects OOB index");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::OutOfRangeCluster,
               "OOB cluster reports out_of_range_cluster reason");
    expectTrue(std::strcmp(fuse::renderer::clusterLookupRejectReasonLabel(reason), "out_of_range_cluster") == 0,
               "lookup reject label for out_of_range_cluster");

    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLights(grid, 0u, tryLights, tryCount),
               "tryLookupClusterLights succeeds on in-range index");
    expectTrue(tryCount == 2u, "tryLookupClusterLights reports cluster light count");
    expectTrue(tryLights.size() == 2u && tryLights[0] == 0u && tryLights[1] == 2u,
               "tryLookupClusterLights copies assigned lights");

    std::vector<fuse::u32> rejectedLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLights(grid, 99u, rejectedLights, rejectedCount, reason),
               "tryLookupClusterLights rejects OOB index");
    expectTrue(rejectedCount == 0u, "tryLookupClusterLights zeroes count on failure");
    expectTrue(rejectedLights.empty(), "tryLookupClusterLights clears output on failure");


    fuse::renderer::ClusterLightGridLayout::rebuildLightGridForDesc(grid, desc, perClusterLights, 2u);

    expectTrue(fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, desc),
               "hasAssignedLightsForDesc true when lights assigned");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, desc) == 3u,
               "countNonEmptyClustersForDesc matches non-empty clusters");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, desc) == 1u,
               "countEmptyClustersForDesc matches empty clusters");
    expectTrue(fuse::renderer::cluster_util::countClustersAtCapacityForDesc(grid, desc, 2u) == 2u,
               "countClustersAtCapacityForDesc matches capacity-clamped clusters");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCountsForDesc(grid, desc),
               "validatePopulationCountsForDesc accepts rebuilt grid");
    expectTrue(!fuse::renderer::cluster_util::shouldSkipLightGridLookup(grid, desc),
               "lookup not skipped when grid has assigned lights");

    fuse::renderer::ClusterGridSoA emptyScene{};
    emptyScene.allocate(desc);
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(emptyScene, desc),
               "hasAssignedLightsForDesc false on allocated but unpopulated grid");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(emptyScene, desc) == 0u,
               "countNonEmptyClustersForDesc zero on unpopulated grid");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(emptyScene, desc) == desc.clusterCount(),
               "countEmptyClustersForDesc matches cluster count on unpopulated grid");
    expectTrue(fuse::renderer::cluster_util::shouldSkipLightGridLookup(emptyScene, desc),
               "lookup skipped when no lights assigned");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, mismatched) == 0u,
               "countNonEmptyClustersForDesc rejects desc mismatch");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, mismatched) == 0u,
               "countEmptyClustersForDesc rejects desc mismatch");
    expectTrue(fuse::renderer::cluster_util::countClustersAtCapacityForDesc(grid, mismatched, 2u) == 0u,
               "countClustersAtCapacityForDesc rejects desc mismatch");
    expectTrue(!fuse::renderer::cluster_util::validatePopulationCountsForDesc(grid, mismatched),
               "validatePopulationCountsForDesc rejects desc mismatch");

    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(emptyGrid, zeroDesc) == 0u,
               "countNonEmptyClustersForDesc zero on empty desc");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(emptyGrid, zeroDesc) == 0u,
               "countEmptyClustersForDesc zero on empty desc");
    expectTrue(fuse::renderer::cluster_util::validatePopulationCountsForDesc(emptyGrid, zeroDesc),
               "validatePopulationCountsForDesc vacuously succeeds on empty desc");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, mismatched),
               "hasAssignedLightsForDesc rejects desc mismatch");
               "countNonEmptyClustersForDesc zero on desc mismatch");
               "countEmptyClustersForDesc zero on desc mismatch");
    expectTrue(fuse::renderer::cluster_util::shouldSkipLightGridLookup(grid, mismatched),
               "lookup skipped on desc mismatch");

    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(emptyGrid, desc),
               "hasAssignedLightsForDesc false on empty storage");
    expectTrue(fuse::renderer::cluster_util::shouldSkipLightGridLookup(emptyGrid, desc),
               "lookup skipped on empty storage");

void testClusterCoordLookupRejectReasons() {

    fuse::renderer::ClusterRebuildRejectReason tryRebuildReason = fuse::renderer::ClusterRebuildRejectReason::None;
    const fuse::u32 dropped =
        fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(grid, desc, perClusterLights,
                                                                           tryRebuildReason, 2u);
    expectTrue(dropped == 0u, "tryRebuildForDesc drops nothing when within capacity");
    expectTrue(tryRebuildReason == fuse::renderer::ClusterRebuildRejectReason::None,
               "successful tryRebuild clears reject reason");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulationForDesc(grid, desc),
               "tryRebuildForDesc produces valid population");

    fuse::renderer::ClusterGridSoA rejectedGrid{};
    rejectedGrid.grid.resize(clusterCount);
    rejectedGrid.lightList = {0u, 1u};
    const fuse::u32 rejectedDropped =
        fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(rejectedGrid, zeroDesc, perClusterLights,
    expectTrue(rejectedDropped == 0u, "tryRebuildForDesc returns zero on guard failure");
    expectTrue(tryRebuildReason == fuse::renderer::ClusterRebuildRejectReason::EmptyGrid,
               "tryRebuildForDesc preserves rebuild reject reason");
    expectTrue(rejectedGrid.lightList.size() == 2u, "tryRebuildForDesc does not mutate grid on guard failure");

void testClusterScreenDepthLookupGuards() {



    expectTrue(fuse::renderer::cluster_util::tryCanLookupAtCoord(grid, desc, 0u, 0u, 0u, reason),
               "tryCanLookupAtCoord accepts accessible grid");
               "accessible grid reports no coord lookup reject reason");

    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryLights, tryCount,
               "tryCoordLookup with reason succeeds on accessible grid");
    expectTrue(tryCount == 2u, "tryCoordLookup with reason reports cluster light count");
               "successful coord lookup clears reject reason");

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtCoord(grid, zeroDesc, 0u, 0u, 0u, reason),
               "tryCanLookupAtCoord rejects empty grid desc");
               "empty grid desc reports empty_grid coord reject reason");

    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtCoord(emptyGrid, desc, 0u, 0u, 0u, reason),
               "tryCanLookupAtCoord rejects empty storage");
               "empty storage reports empty_storage coord reject reason");

    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::None, "matching rebuild reports no reject reason");

               "tryCanRebuild accepts zero cluster count");
    expectTrue(reason == fuse::renderer::GridRebuildRejectReason::None, "zero cluster count reports no reject reason");

               "oversized cluster count reports cluster_count_mismatch reason");
               "rebuild reject label for cluster_count_mismatch");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, 4u, reason),
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(zeroDesc),
               "shouldSkipLightGridRebuild true on empty desc");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc),
               "shouldSkipLightGridRebuild false on non-empty desc");

void testClusterScreenMappingRejectReasons() {
void testClusterScreenMappingRejectGuards() {
    fuse::renderer::ClusterDesc desc{};
    desc.tilesX = 4;
    desc.tilesY = 2;
    desc.slicesZ = 3;

    fuse::renderer::ClusterCameraDesc camera{};
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;

    std::vector<fuse::u32> screenLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtScreenDepth(grid, desc, camera, 0.25f, 0.25f, 10.f,
                                                                              screenLights) == 2u,
               "screen-depth lookup returns cluster count");
    expectTrue(screenLights.size() == 2u && screenLights[0] == 0u && screenLights[1] == 1u,
               "screen-depth lookup copies assigned lights");

    expectTrue(fuse::renderer::cluster_util::clusterLightCountAtScreenDepth(grid, desc, camera, 0.25f, 0.25f, 10.f) ==
                   2u,
               "screen-depth count matches lookup");

    fuse::u32 tryScreenCount = 0u;
    std::vector<fuse::u32> tryScreenLights;
    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtScreenDepth(grid, desc, camera, 0.25f, 0.25f, 10.f,
                                                                                 tryScreenLights, tryScreenCount),
               "tryScreenDepthLookup succeeds on accessible grid");
    expectTrue(tryScreenCount == 2u, "tryScreenDepthLookup reports cluster light count");

    fuse::renderer::ClusterLookupRejectReason lookupReason = fuse::renderer::ClusterLookupRejectReason::None;
                                                                                 tryScreenLights, tryScreenCount,
                                                                                 lookupReason),
               "tryScreenDepthLookup with reason succeeds on accessible grid");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::None,
               "successful screen-depth lookup clears reject reason");

    std::vector<fuse::u32> outOfRangeLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtScreenDepth(grid, desc, camera, 0.5f, 0.5f, 0.01f,
                                                                              outOfRangeLights) == 0u,
               "screen-depth lookup rejects depth below near plane");
    expectTrue(outOfRangeLights.empty(), "screen-depth lookup clears output on mapping failure");

    fuse::u32 rejectedScreenCount = 0u;
    std::vector<fuse::u32> rejectedScreenLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtScreenDepth(grid, desc, camera, 0.5f, 0.5f, 0.01f,
                                                                                  rejectedScreenLights,
                                                                                  rejectedScreenCount),
               "tryScreenDepthLookup rejects depth below near plane");
    expectTrue(rejectedScreenCount == 0u, "tryScreenDepthLookup zeroes count on mapping failure");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    std::vector<fuse::u32> emptyStorageLights;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtScreenDepth(emptyGrid, desc, camera, 0.5f, 0.5f,
                                                                              10.f, emptyStorageLights) == 0u,
               "screen-depth lookup rejects empty storage");
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtScreenDepth(emptyGrid, desc, camera, 0.5f, 0.5f,
                                                                                  10.f, rejectedScreenLights,
                                                                                  rejectedScreenCount, lookupReason),
               "tryScreenDepthLookup with reason rejects empty storage");
    expectTrue(lookupReason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "empty storage screen-depth lookup reports empty_storage reason");

    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(fuse::renderer::cluster_util::lookupClusterLightsAtScreenDepth(grid, zeroDesc, camera, 0.5f, 0.5f, 10.f,
                                                                              emptyStorageLights) == 0u,
               "screen-depth lookup rejects empty grid desc");
}

void testClusterPopulationAndCoordLookupDeepen() {
    fuse::u32 clusterIndex = 0u;
    fuse::renderer::ClusterScreenMappingRejectReason mapReason =
        fuse::renderer::ClusterScreenMappingRejectReason::None;
    expectTrue(fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, desc, camera, clusterIndex, mapReason),
               "tryMapScreenDepth accepts in-range depth");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::None,
               "in-range depth reports no reject reason");
    expectTrue(clusterIndex < desc.clusterCount(), "mapped cluster index in bounds");

    fuse::u32 emptyIndex = 99u;
    expectTrue(!fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, emptyIndex, mapReason),
               "tryMapScreenDepth rejects empty grid");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::EmptyGrid,
               "empty grid reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::clusterScreenMappingRejectReasonLabel(mapReason), "empty_grid") == 0,
               "screen mapping reject label for empty grid");

    fuse::u32 depthIndex = 0u;
                   0.5f, 0.5f, 200.f, desc, camera, depthIndex, mapReason),
               "tryMapScreenDepth rejects depth above far plane");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::DepthOutOfRange,
               "depth above far reports depth_out_of_range reason");

    fuse::renderer::ClusterCameraDesc invalidCamera{};
    invalidCamera.nearPlane = 0.f;
    invalidCamera.farPlane = 100.f;
    fuse::u32 invalidIndex = 0u;
                   0.5f, 0.5f, 10.f, desc, invalidCamera, invalidIndex, mapReason),
               "tryMapScreenDepth rejects invalid camera");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera reason");
    expectTrue(std::strcmp(fuse::renderer::clusterScreenMappingRejectReasonLabel(mapReason), "invalid_camera") == 0,
               "screen mapping reject label for invalid camera");

void testClusterRebuildPreflightGuards() {
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

    fuse::renderer::GridRebuildRejectReason rebuildReason = fuse::renderer::GridRebuildRejectReason::None;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount, rebuildReason),
               "tryCanRebuild accepts matching cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::None,
               "matching cluster count reports no reject reason");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, clusterCount + 1u, rebuildReason),
               "tryCanRebuild rejects oversized cluster count");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::ClusterCountMismatch,
               "oversized cluster count reports cluster_count_mismatch reason");
    expectTrue(std::strcmp(fuse::renderer::gridRebuildRejectReasonLabel(rebuildReason), "cluster_count_mismatch") ==
                   0,
               "rebuild reject label for cluster count mismatch");

    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(zeroDesc, 4u, rebuildReason),
               "tryCanRebuild rejects non-zero count on empty desc");
    expectTrue(rebuildReason == fuse::renderer::GridRebuildRejectReason::EmptyGrid,
               "empty desc reports empty_grid rebuild reason");

    expectTrue(fuse::renderer::ClusterLightGridLayout::tryCanRebuildLightGrid(desc, 0u, rebuildReason),
               "tryCanRebuild vacuously succeeds for zero cluster count");
               "zero cluster count reports no rebuild reject reason");

    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, clusterCount + 1u),
               "shouldSkip rebuild on cluster count mismatch");
    expectTrue(!fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(desc, 0u),
               "shouldSkip rebuild does not block zero cluster count clear");
    expectTrue(fuse::renderer::ClusterLightGridLayout::shouldSkipLightGridRebuild(zeroDesc, 4u),
               "shouldSkip rebuild on empty desc with non-zero count");

    fuse::renderer::ClusterGridSoA grid{};
    fuse::u32 dropped = 0u;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(
                   grid, desc, clusterCount, perClusterLights, 2u, dropped, rebuildReason),
               "tryRebuild succeeds on valid preflight");
    expectTrue(dropped == 0u, "tryRebuild drops nothing when within capacity");
               "successful rebuild clears reject reason");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulation(grid, clusterCount),
               "tryRebuild grid passes population validation");

    fuse::u32 rejectedDropped = 99u;
    expectTrue(!fuse::renderer::ClusterLightGridLayout::tryRebuildLightGrid(
                   grid, desc, clusterCount + 1u, perClusterLights, 2u, rejectedDropped, rebuildReason),
               "tryRebuild rejects invalid preflight");
    expectTrue(rejectedDropped == 0u, "tryRebuild zeroes dropped count on failure");
               "failed rebuild preserves reject reason");

    fuse::renderer::ClusterGridSoA descGrid{};
    fuse::u32 descDropped = 0u;
    expectTrue(fuse::renderer::ClusterLightGridLayout::tryRebuildLightGridForDesc(
                   descGrid, desc, perClusterLights, 2u, descDropped, rebuildReason),
               "tryRebuildForDesc succeeds on valid preflight");
    expectTrue(descDropped == 0u, "tryRebuildForDesc drops nothing when within capacity");
    expectTrue(fuse::renderer::cluster_util::validateGridPopulationForDesc(descGrid, desc),
               "tryRebuildForDesc grid passes population validation");

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

    expectTrue(!fuse::renderer::cluster_util::shouldSkipClusterPopulation(grid, desc),
               "accessible grid does not skip population validation");
    expectTrue(fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, desc),
               "hasAssignedLightsForDesc true on populated grid");

    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterPopulation(emptyGrid, desc),
               "empty storage skips population validation");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(emptyGrid, desc),
               "hasAssignedLightsForDesc false on empty storage");

    expectTrue(!fuse::renderer::cluster_util::shouldSkipClusterPopulation(emptyGrid, zeroDesc),
               "empty desc does not skip population validation");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtCoord(grid, mismatched, 0u, 0u, 0u, reason),
               "tryCanLookupAtCoord rejects desc mismatch");
    expectTrue(reason == fuse::renderer::ClusterLookupRejectReason::DescMismatch,
               "desc mismatch reports desc_mismatch coord reject reason");

    fuse::u32 rejectedCount = 0u;
    std::vector<fuse::u32> rejectedLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(emptyGrid, desc, 0u, 0u, 0u, rejectedLights,
                                                                           rejectedCount, reason),
               "tryCoordLookup with reason rejects empty storage");
    expectTrue(rejectedCount == 0u, "tryCoordLookup with reason zeroes count on failure");
    expectTrue(rejectedLights.empty(), "tryCoordLookup with reason clears output on failure");
    expectTrue(fuse::renderer::cluster_util::shouldSkipClusterPopulation(grid, mismatched),
               "desc mismatch skips population validation");
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, mismatched),
               "hasAssignedLightsForDesc false on desc mismatch");

    fuse::u32 tryCoordCount = 0u;
    std::vector<fuse::u32> tryCoordLights;
    fuse::renderer::ClusterLookupRejectReason coordReason = fuse::renderer::ClusterLookupRejectReason::None;
    expectTrue(fuse::renderer::cluster_util::canLookupAtCoord(grid, desc),
               "canLookupAtCoord accepts accessible grid");
               "canLookupAtCoord ignores tile/slice when storage matches desc");

    expectTrue(fuse::renderer::cluster_util::tryCanLookupAtCoord(grid, desc, coordReason),
               "tryCanLookupAtCoord accepts accessible grid");
    expectTrue(coordReason == fuse::renderer::ClusterLookupRejectReason::None,
               "accessible grid reports no coord lookup reject reason");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    expectTrue(!fuse::renderer::cluster_util::tryCanLookupAtCoord(emptyGrid, desc, coordReason),
               "tryCanLookupAtCoord rejects empty storage");
    expectTrue(coordReason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "empty storage reports empty_storage coord reason");

    expectTrue(fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(grid, desc, 0u, 0u, 0u, tryCoordLights,
                                                                           tryCoordCount, coordReason),
               "tryCoordLookup with reason succeeds on accessible grid");
    expectTrue(tryCoordCount == 2u, "tryCoordLookup with reason reports cluster light count");
    expectTrue(coordReason == fuse::renderer::ClusterLookupRejectReason::None,
               "successful coord lookup clears reject reason");

    fuse::u32 rejectedCoordCount = 0u;
    std::vector<fuse::u32> rejectedCoordLights;
    expectTrue(!fuse::renderer::cluster_util::tryLookupClusterLightsAtCoord(emptyGrid, desc, 0u, 0u, 0u,
                                                                            rejectedCoordLights, rejectedCoordCount,
                                                                            coordReason),
    expectTrue(rejectedCoordCount == 0u, "tryCoordLookup with reason zeroes count on failure");
    expectTrue(coordReason == fuse::renderer::ClusterLookupRejectReason::EmptyStorage,
               "failed coord lookup preserves reject reason");
    fuse::u32 clusterIndex = 0u;
    fuse::renderer::ClusterScreenMappingRejectReason mapReason =
        fuse::renderer::ClusterScreenMappingRejectReason::None;
    expectTrue(fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, desc, camera, clusterIndex, mapReason),
               "tryMapScreenDepthToClusterIndex succeeds in range");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::None,
               "in-range mapping reports no reject reason");
    expectTrue(clusterIndex < desc.clusterCount(), "mapped cluster index in bounds");

    expectTrue(!fuse::renderer::ClusterGridLayout::tryMapScreenDepthToClusterIndex(
                   0.5f, 0.5f, 10.f, zeroDesc, camera, clusterIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects empty grid");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::EmptyGrid,
               "empty grid reports empty_grid mapping reason");
    expectTrue(std::strcmp(fuse::renderer::clusterScreenMappingRejectReasonLabel(mapReason), "empty_grid") == 0,
               "screen mapping reject label for empty grid");

                   0.5f, 0.5f, 0.01f, desc, camera, clusterIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects depth below near plane");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::DepthOutOfRange,
               "below-near depth reports depth_out_of_range reason");

    fuse::renderer::ClusterCameraDesc invalidCamera{};
    invalidCamera.nearPlane = 0.f;
    invalidCamera.farPlane = 100.f;
                   0.5f, 0.5f, 10.f, desc, invalidCamera, clusterIndex, mapReason),
               "tryMapScreenDepthToClusterIndex rejects invalid camera");
    expectTrue(mapReason == fuse::renderer::ClusterScreenMappingRejectReason::InvalidCamera,
               "invalid camera reports invalid_camera reason");
               "tryCoordLookup with reason rejects empty storage");
    expectTrue(rejectedCoordLights.empty(), "tryCoordLookup with reason clears output on failure");
}

void testClusterPopulationForDescGuards() {
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

    expectTrue(fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, desc),
               "hasAssignedLightsForDesc true on populated grid");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, desc) == 3u,
               "countNonEmptyClustersForDesc matches non-empty clusters");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, desc) == 1u,
               "countEmptyClustersForDesc matches empty clusters");

    fuse::renderer::ClusterDesc mismatched{};
    mismatched.tilesX = 1;
    mismatched.tilesY = 1;
    mismatched.slicesZ = 1;
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(grid, mismatched),
               "hasAssignedLightsForDesc rejects desc mismatch");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(grid, mismatched) == 0u,
               "countNonEmptyClustersForDesc zero on desc mismatch");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(grid, mismatched) == 0u,
               "countEmptyClustersForDesc zero on desc mismatch");

    fuse::renderer::ClusterGridSoA emptyGrid{};
    fuse::renderer::ClusterDesc zeroDesc{};
    zeroDesc.tilesX = 0u;
    expectTrue(!fuse::renderer::cluster_util::hasAssignedLightsForDesc(emptyGrid, zeroDesc),
               "hasAssignedLightsForDesc false on empty desc");
    expectTrue(fuse::renderer::cluster_util::countNonEmptyClustersForDesc(emptyGrid, zeroDesc) == 0u,
               "countNonEmptyClustersForDesc zero on empty desc");
    expectTrue(fuse::renderer::cluster_util::countEmptyClustersForDesc(emptyGrid, zeroDesc) == 0u,
               "countEmptyClustersForDesc zero on empty desc");
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
    testClusterCoordValidationAndTryIndex();
    testStrictLookupAndPopulationHelpers();
    testLightListBoundsValidation();
    testClusterLookupAtIndexGuards();
    testClusterGridAccessibilityAndSkipGuards();
    testClusterLookupRejectReasons();
    testClusterDirectLookupGuards();
    testClusterPopulationStateGuards();
    testClusterDescScopedPopulationCounts();
    testClusterLightGridRebuildSkipGuard();
    testClusterDescScopedGridHelpers();
    testClusterRebuildPreflightGuards();
    testClusterContiguousOffsetPreflight();
    testClusterCoordLookupRejectReasons();
    testClusterPopulationSkipAndDescHelpers();
    testClusterCoordLookupAndRebuildGuards();
    testClusterScreenLookupAndPopulationDescGuards();
    testClusterLightGridAccessibilityGuards();
    testClusterLookupAtCoordGuards();
    testClusterLookupFromScreenGuards();
    testClusterRebuildAndPopulationPreflightGuards();
    testClusterCoordLookupRebuildAndPopulationGuards();
    testClusterRebuildPreflightGuards();
    testClusterCoordLookupRejectReasons();
    testClusterContiguousOffsetPreflightGuards();
    testClusterCoordLookupRejectReasonAndScreenMapping();
    testClusterScreenDepthLookupGuards();
    testClusterPopulationAndCoordLookupDeepen();
    testClusterScreenDepthMappingGuards();
    testClusterScreenMappingRejectReasons();
    testGridRebuildRejectReasons();
    testClusterCoordLookupRejectReasonsAndSkipGrid();
    testValidateGridPopulationDeepen();
    testClusterGridAccessibleGuard();
    testClusterLookupAtCoordGuards();
    testClusterLookupFromScreenGuards();
    testTryValidateGridPopulationForDesc();
    testClusterLightGridAccessAndCoordLookupGuards();
    testClusterAssignedLightingGuards();
    testClusterDescScopedPopulationCounts();
    testClusterRebuildPreflightGuards();
    testClusterDescScopedPopulationGuards();
    testClusterCoordLookupRejectReasons();
    testClusterLookupAtCoordRejectReasons();
    testClusterDirectLookupGuards();
    testClusterScreenMappingRejectReasons();
    testClusterScreenMappingRejectGuards();
    testClusterCoordLookupPreflightGuards();
    testClusterPopulationForDescGuards();
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
