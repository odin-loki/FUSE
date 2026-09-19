#include <fuse/core/init.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_kernels.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(desc) == 23u, "maxProbeIndex is last probe");
    expectTrue(fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(23u, desc), "last probe index at max");
    expectTrue(!fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(22u, desc), "interior index not at max");

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

void testMaxProbeIndexAndTryClamp() {
void testProbeMaxIndexHelpers() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {4, 2, 3};

    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(desc) == 23u,
               "maxProbeIndex is last flat index");
    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(desc) ==
                   fuse::renderer::ddgi_util::probeCount(desc) - 1u,
               "maxProbeIndex is probeCount - 1");

    fuse::u32 clampedIndex = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeIndex(17u, desc, clampedIndex),
               "tryClamp succeeds on non-empty grid");
    expectTrue(clampedIndex == 17u, "tryClamp preserves in-bounds index");

    fuse::u32 clampedOob = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeIndex(999u, desc, clampedOob),
               "tryClamp succeeds when clamping OOB index");
    expectTrue(clampedOob == fuse::renderer::ProbeGridLayout::maxProbeIndex(desc),
               "tryClamp clamps OOB index to max");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::u32 emptyClamp = 99u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeIndex(5u, empty, emptyClamp),
               "tryClamp rejects empty grid");
    expectTrue(emptyClamp == 0u, "tryClamp zeroes output on empty grid");
               "maxProbeIndex returns last valid probe index");
    expectTrue(fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(23u, desc),
               "last probe index flagged at max");
    expectTrue(!fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(22u, desc),
               "non-last probe index not at max");

    fuse::u32 clamped = 99u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeIndex(17u, desc, clamped),
               "tryClampProbeIndex succeeds on non-empty grid");
    expectTrue(clamped == 17u, "tryClampProbeIndex leaves in-range index unchanged");

    fuse::u32 oobClamped = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeIndex(999u, desc, oobClamped),
               "tryClampProbeIndex succeeds for OOB index");
    expectTrue(oobClamped == 23u, "tryClampProbeIndex clamps OOB index to max");

    fuse::u32 emptyClamped = 5u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeIndex(5u, empty, emptyClamped),
               "tryClampProbeIndex rejects empty grid");
    expectTrue(emptyClamped == 0u, "tryClampProbeIndex zeroes output on empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(empty) == 0u,
               "maxProbeIndex zero on empty grid");
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
    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeIndex(UINT32_MAX, desc) == 23u,
               "UINT32_MAX probe index clamped to last probe");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeIndexOutOfRange(17u, desc),
               "in-range probe index not out of range");
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeIndexOutOfRange(999u, desc),
               "OOB probe index flagged out of range");

    const fuse::renderer::ProbeGridCoord clampedCoord =
        fuse::renderer::ProbeGridLayout::clampProbeGridCoord(desc, {9, 9, 9});
    expectTrue(clampedCoord.x == 3u && clampedCoord.y == 1u && clampedCoord.z == 2u,
               "probe coord clamped to grid max per axis");

    const fuse::renderer::ProbeGridCoord fromClamped =
        fuse::renderer::ProbeGridLayout::probeCoordFromClampedIndex(desc, 999u);
    expectTrue(fromClamped.x == 3u && fromClamped.y == 1u && fromClamped.z == 2u,
               "probeCoordFromClampedIndex maps OOB index to last coord");

    const fuse::renderer::ProbeValidityFlags clampedValidity =
        fuse::renderer::ProbeGridLayout::probeValidityFromClampedIndex(desc, 999u);
    expectTrue(clampedValidity.valid, "clamped validity valid for OOB index");
    expectTrue(clampedValidity.is_border, "clamped OOB index maps to border corner");
}

void testProbeSampleCoords() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords centre{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, centre),
               "buildProbeSampleCoords succeeds on interior sample");
    expectTrue(centre.x0 == 0u && centre.x1 == 1u, "centre sample spans x0/x1");
    expectNear(centre.tx, 0.5f, 1e-5f, "centre tx is 0.5");

    fuse::renderer::ProbeSampleCoords border{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.f, 0.f, 0.f}, border),
               "buildProbeSampleCoords succeeds on border sample");
    expectTrue(border.x0 == 0u && border.x1 == 1u && border.y0 == 0u && border.y1 == 1u,
               "border sample still has neighbour indices for 2x2x2 grid");
    expectNear(border.tx, 0.f, 1e-5f, "border tx is 0");

    fuse::renderer::ProbeSampleCoords oob{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {100.f, 100.f, 100.f}, oob),
               "buildProbeSampleCoords succeeds on OOB high sample");
    expectTrue(oob.x0 == 1u && oob.x1 == 1u && oob.y0 == 1u && oob.y1 == 1u && oob.z0 == 1u && oob.z1 == 1u,
               "OOB high sample clamps to max corner indices");
}

void testEmptyProbeGrid() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {0, 8, 16};

    expectTrue(fuse::renderer::ProbeGridLayout::isEmptyGrid(desc), "zero x dimension is empty grid");
    expectTrue(fuse::renderer::ddgi_util::probeCount(desc) == 0u, "zero-dimension grid has zero probes");
    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeIndex(5u, desc) == 0u,
               "empty grid index clamp returns 0");
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeIndexOutOfRange(0u, desc),
               "any index out of range on empty grid");
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeIndex(desc, 0u), "index 0 invalid on empty grid");

    const fuse::renderer::ProbeGridCoord clampedEmpty =
        fuse::renderer::ProbeGridLayout::probeCoordFromClampedIndex(desc, 99u);
    expectTrue(clampedEmpty.x == 0u && clampedEmpty.y == 0u && clampedEmpty.z == 0u,
               "empty grid clamped coord is origin");

    fuse::renderer::ProbeSampleCoords emptyCoords{};
    expectTrue(!fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.f, 0.f, 0.f}, emptyCoords),
               "empty grid buildProbeSampleCoords returns false");

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

    const fuse::renderer::ProbeValidityFlags emptyClampedValidity =
        fuse::renderer::ProbeGridLayout::probeValidityFromClampedIndex(desc, 99u);
    expectTrue(!emptyClampedValidity.valid, "clamped validity invalid on empty grid");

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
    expectTrue(cornerFlags.border_kind == fuse::renderer::ProbeBorderKind::Corner, "corner border kind");
    expectTrue(!cornerFlags.has_trilinear_neighbourhood, "corner lacks trilinear neighbourhood");

    const fuse::renderer::ProbeGridCoord faceCenter{0, 1, 1};
    const fuse::renderer::ProbeValidityFlags faceFlags =
        fuse::renderer::ProbeGridLayout::probeValidity(desc, faceCenter);
    expectTrue(faceFlags.valid, "face-center probe valid");
    expectTrue(faceFlags.is_border, "face-center probe is border");
    expectTrue(!faceFlags.interior, "face-center probe not interior");
    expectTrue(faceFlags.border_kind == fuse::renderer::ProbeBorderKind::Face, "face border kind");
    expectTrue(!faceFlags.has_trilinear_neighbourhood, "face-center lacks trilinear neighbourhood");

    const fuse::renderer::ProbeGridCoord edge{0, 0, 1};
    const fuse::renderer::ProbeValidityFlags edgeFlags =
        fuse::renderer::ProbeGridLayout::probeValidity(desc, edge);
    expectTrue(edgeFlags.valid, "edge probe valid");
    expectTrue(edgeFlags.is_border, "edge probe is border");
    expectTrue(edgeFlags.border_kind == fuse::renderer::ProbeBorderKind::Edge, "edge border kind");
    expectTrue(!edgeFlags.has_trilinear_neighbourhood, "edge lacks trilinear neighbourhood");

    const fuse::renderer::ProbeGridCoord interior{1, 1, 1};
    const fuse::renderer::ProbeValidityFlags interiorFlags =
        fuse::renderer::ProbeGridLayout::probeValidity(desc, interior);
    expectTrue(interiorFlags.valid, "interior probe valid");
    expectTrue(!interiorFlags.is_border, "interior probe not border");
    expectTrue(interiorFlags.interior, "interior probe flagged interior");
    expectTrue(interiorFlags.border_kind == fuse::renderer::ProbeBorderKind::Interior, "interior border kind");
    expectTrue(interiorFlags.has_trilinear_neighbourhood, "interior has trilinear neighbourhood");

    fuse::renderer::DDGIDesc single{};
    single.grid_dims = {1, 1, 1};
    const fuse::renderer::ProbeValidityFlags loneProbe =
        fuse::renderer::ProbeGridLayout::probeValidity(single, {0, 0, 0});
    expectTrue(loneProbe.valid, "1x1x1 lone probe valid");
    expectTrue(loneProbe.is_border, "1x1x1 lone probe is border");
    expectTrue(loneProbe.border_kind == fuse::renderer::ProbeBorderKind::Corner, "1x1x1 lone probe is corner");
    expectTrue(!loneProbe.has_trilinear_neighbourhood, "1x1x1 lacks trilinear neighbourhood");

    const fuse::renderer::ProbeValidityFlags fromIndex =
        fuse::renderer::ProbeGridLayout::probeValidityFromIndex(desc, 13u);
    expectTrue(fromIndex.valid, "index 13 validity from index helper");
    expectTrue(fromIndex.interior, "index 13 is interior in 3x3x3 grid");

    const fuse::renderer::ProbeValidityFlags invalidIndex =
        fuse::renderer::ProbeGridLayout::probeValidityFromIndex(desc, 99u);
    expectTrue(!invalidIndex.valid, "out-of-range index invalid");
}

void testProbePerAxisClamp() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {4, 2, 3};

    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeCoordX(99u, desc) == 3u, "clamp probe X");
    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeCoordY(99u, desc) == 1u, "clamp probe Y");
    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeCoordZ(99u, desc) == 2u, "clamp probe Z");

    const fuse::u32 clampedIndex =
        fuse::renderer::ProbeGridLayout::probeIndexFromClampedCoord(desc, {9, 9, 9});
    expectTrue(clampedIndex == 23u, "probeIndexFromClampedCoord maps OOB coord to last index");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::probeIndexFromClampedCoord(empty, {1, 1, 1}) == 0u,
               "empty grid clamped coord index is 0");
    expectTrue(fuse::renderer::ProbeGridLayout::clampProbeCoordX(5u, empty) == 0u,
               "empty grid clamp probe X returns 0");
}

void testProbeSampleCoordValidation() {
void testProbeSampleCoordGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "buildProbeSampleCoords succeeds for interior sample");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, built),
               "built interior sample coords are valid");

    fuse::renderer::ProbeSampleCoords clampedBuilt{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildAndClampProbeSampleCoords(
                   desc, {100.f, 100.f, 100.f}, clampedBuilt),
               "buildAndClampProbeSampleCoords succeeds on OOB world position");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, clampedBuilt),
               "buildAndClamp output passes validation");

    fuse::renderer::ProbeSampleCoords invalid{};
    invalid.x0 = 5u;
    invalid.x1 = 1u;
    invalid.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalid),
               "OOB indices and weights fail validation");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::renderer::ProbeSampleCoords emptyBuilt{};
    expectTrue(!fuse::renderer::ProbeGridLayout::buildAndClampProbeSampleCoords(
                   empty, {0.f, 0.f, 0.f}, emptyBuilt),
               "buildAndClamp fails on empty grid");
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(empty, built),
               "sample coord validation fails on empty grid");
               "built sample coords pass validation");

    fuse::renderer::ProbeSampleCoords invalidWeights = built;
    invalidWeights.tx = 1.5f;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalidWeights),
               "sample coords with OOB weight rejected");

    fuse::renderer::ProbeSampleCoords invalidNeighbour = built;
    invalidNeighbour.x1 = built.x0 + 2u;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalidNeighbour),
               "sample coords with distant neighbour rejected");

    fuse::renderer::ProbeSampleCoords dirty{};
    dirty.x0 = 99u;
    dirty.y0 = 99u;
    dirty.z0 = 99u;
    dirty.x1 = 99u;
    dirty.y1 = 99u;
    dirty.z1 = 99u;
    dirty.tx = 2.f;
    dirty.ty = -1.f;
    dirty.tz = 0.25f;
    fuse::renderer::ProbeGridLayout::sanitizeProbeSampleCoords(desc, dirty);
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, dirty),
               "sanitize restores valid trilinear neighbourhood");
    expectTrue(dirty.x0 == 1u && dirty.x1 == 1u, "sanitize clamps high corner indices");
    expectNear(dirty.tx, 1.f, 1e-5f, "sanitize clamps high blend weight");
    expectNear(dirty.ty, 0.f, 1e-5f, "sanitize clamps low blend weight");

    fuse::renderer::ProbeSampleCoords huge{};
    huge.x0 = UINT32_MAX;
    huge.y0 = UINT32_MAX;
    huge.z0 = UINT32_MAX;
    fuse::renderer::ProbeGridLayout::sanitizeProbeSampleCoords(desc, huge);
    expectTrue(huge.x0 == 1u && huge.y0 == 1u && huge.z0 == 1u,
               "sanitize clamps UINT32_MAX corner indices to grid max");

    fuse::renderer::ProbeSampleCoords emptyCoords{};
               "empty grid sample coords invalid");
    fuse::renderer::ProbeGridLayout::sanitizeProbeSampleCoords(empty, emptyCoords);
    expectTrue(emptyCoords.x0 == 0u && emptyCoords.x1 == 0u, "empty grid sanitize clears sample coords");
void testProbeSampleCoordsGuards() {

    fuse::renderer::ProbeSampleCoords valid{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, valid),
               "buildProbeSampleCoords succeeds for guard validation");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, valid),
               "built sample coords pass validity guard");

    invalid.x0 = 9u;
    invalid.y0 = 9u;
    invalid.z0 = 9u;
    invalid.x1 = 9u;
    invalid.y1 = 9u;
    invalid.z1 = 9u;
    invalid.ty = -1.f;
    invalid.tz = 0.5f;
               "OOB sample coords fail validity guard");
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, invalid),
               "tryClampProbeSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalid),
               "clamped sample coords pass validity guard");
    expectTrue(invalid.x0 == 1u && invalid.x1 == 1u, "tryClamp clamps x indices");

    emptyCoords.x0 = 3u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(empty, emptyCoords),
               "tryClampProbeSampleCoords fails on empty grid");
    expectTrue(emptyCoords.x0 == 0u && emptyCoords.x1 == 0u, "empty grid tryClamp clears coords");
}

void testClampProbeSampleCoords() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords coords{};
    coords.x0 = 9u;
    coords.y0 = 9u;
    coords.z0 = 9u;
    coords.x1 = 9u;
    coords.y1 = 9u;
    coords.z1 = 9u;
    coords.tx = 2.f;
    coords.ty = -1.f;
    coords.tz = 0.5f;
    fuse::renderer::ProbeGridLayout::clampProbeSampleCoords(desc, coords);
    expectTrue(coords.x0 == 1u && coords.x1 == 1u, "clamp sample coord x indices");
    expectTrue(coords.y0 == 1u && coords.y1 == 1u, "clamp sample coord y indices");
    expectTrue(coords.z0 == 1u && coords.z1 == 1u, "clamp sample coord z indices");
    expectNear(coords.tx, 1.f, 1e-5f, "clamp sample tx to unit range");
    expectNear(coords.ty, 0.f, 1e-5f, "clamp sample ty to unit range");
    expectNear(coords.tz, 0.5f, 1e-5f, "clamp sample tz unchanged in range");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::renderer::ProbeSampleCoords emptyCoords{};
    emptyCoords.x0 = 3u;
    fuse::renderer::ProbeGridLayout::clampProbeSampleCoords(empty, emptyCoords);
    expectTrue(emptyCoords.x0 == 0u && emptyCoords.x1 == 0u, "empty grid clamp clears sample coords");
}

void testProbeBorderCounts() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {3, 3, 3};
    expectTrue(fuse::renderer::ddgi_util::countBorderProbes(desc) == 26u, "3x3x3 has 26 border probes");
    expectTrue(fuse::renderer::ddgi_util::countInteriorProbes(desc) == 1u, "3x3x3 has 1 interior probe");

    const fuse::renderer::ProbeBorderCounts byKind =
        fuse::renderer::ddgi_util::countProbesByBorderKind(desc);
    expectTrue(byKind.total == 27u, "3x3x3 total probe count");
    expectTrue(byKind.interior == 1u, "3x3x3 interior by kind");
    expectTrue(byKind.border == 26u, "3x3x3 border by kind");
    expectTrue(byKind.face == 6u, "3x3x3 has 6 face probes");
    expectTrue(byKind.edge == 12u, "3x3x3 has 12 edge probes");
    expectTrue(byKind.corner == 8u, "3x3x3 has 8 corner probes");
    expectTrue(byKind.interior + byKind.border == byKind.total, "interior + border equals total");
    expectTrue(byKind.face + byKind.edge + byKind.corner == byKind.border,
               "face + edge + corner equals border count");

    fuse::renderer::DDGIDesc single{};
    single.grid_dims = {1, 1, 1};
    expectTrue(fuse::renderer::ddgi_util::countBorderProbes(single) == 1u, "1x1x1 border count");
    expectTrue(fuse::renderer::ddgi_util::countInteriorProbes(single) == 0u, "1x1x1 interior count");

    const fuse::renderer::ProbeBorderCounts loneKind =
        fuse::renderer::ddgi_util::countProbesByBorderKind(single);
    expectTrue(loneKind.corner == 1u, "1x1x1 lone probe is corner");
    expectTrue(loneKind.interior == 0u, "1x1x1 lone probe has no interior");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 3, 3};
    expectTrue(fuse::renderer::ddgi_util::countBorderProbes(empty) == 0u, "empty grid border count");
    expectTrue(fuse::renderer::ddgi_util::countInteriorProbes(empty) == 0u, "empty grid interior count");
    const fuse::renderer::ProbeBorderCounts emptyKind =
        fuse::renderer::ddgi_util::countProbesByBorderKind(empty);
    expectTrue(emptyKind.total == 0u, "empty grid border-kind total is zero");

    expectTrue(fuse::renderer::ddgi_util::validateProbeBorderCounts(byKind),
               "3x3x3 border counts satisfy invariants");
    expectTrue(fuse::renderer::ddgi_util::validateProbeBorderCounts(emptyKind),
               "empty grid border counts satisfy invariants");
    expectTrue(fuse::renderer::ddgi_util::validateProbeBorderCountsForGrid(desc),
               "3x3x3 grid border counts validate via helper");
    expectTrue(fuse::renderer::ddgi_util::validateProbeBorderCountsForGrid(empty),
               "empty grid border counts validate via helper");
    expectTrue(fuse::renderer::ddgi_util::countProbesOfBorderKind(desc,
                                                                  fuse::renderer::ProbeBorderKind::Interior) == 1u,
               "countProbesOfBorderKind interior");
    expectTrue(fuse::renderer::ddgi_util::countProbesOfBorderKind(desc,
                                                                  fuse::renderer::ProbeBorderKind::Corner) == 8u,
               "countProbesOfBorderKind corner");
    expectTrue(fuse::renderer::ddgi_util::countProbesOfBorderKind(empty,
                                                                  fuse::renderer::ProbeBorderKind::Face) == 0u,
               "countProbesOfBorderKind zero on empty grid");
    expectTrue(fuse::renderer::ddgi_util::countProbesOfBorderKind(desc,
                                                                  fuse::renderer::ProbeBorderKind::Invalid) == 0u,
               "countProbesOfBorderKind Invalid returns zero");

    fuse::renderer::ProbeBorderCounts inconsistent{};
    inconsistent.total = 8u;
    inconsistent.interior = 3u;
    inconsistent.border = 4u;
    expectTrue(!fuse::renderer::ddgi_util::validateProbeBorderCounts(inconsistent),
               "inconsistent interior+border sum fails validation");
}

void testBorderProbeIndexGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {3, 3, 3};

    expectTrue(fuse::renderer::ProbeGridLayout::isBorderProbeIndex(desc, 0u),
               "origin probe index is border");
    expectTrue(fuse::renderer::ProbeGridLayout::isBorderProbeIndex(desc, 26u),
               "last probe index is border");
    expectTrue(!fuse::renderer::ProbeGridLayout::isBorderProbeIndex(desc, 13u),
               "interior probe index is not border");
    expectTrue(!fuse::renderer::ProbeGridLayout::isBorderProbeIndex(desc, 99u),
               "OOB probe index is not border");

    expectTrue(fuse::renderer::ProbeGridLayout::probeBorderKindFromIndex(desc, 0u) ==
                   fuse::renderer::ProbeBorderKind::Corner,
               "origin index is corner kind");
    expectTrue(fuse::renderer::ProbeGridLayout::probeBorderKindFromIndex(desc, 13u) ==
                   fuse::renderer::ProbeBorderKind::Interior,
               "interior index is interior kind");
    expectTrue(fuse::renderer::ProbeGridLayout::probeBorderKindFromIndex(desc, 99u) ==
                   fuse::renderer::ProbeBorderKind::Invalid,
               "OOB index border kind is invalid");
}

void testProbeSampleCoordGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {4, 4, 4};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {1.5f, 1.5f, 1.5f}, built),
               "tryBuildProbeSampleCoords succeeds for interior sample");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, built),
               "built interior sample coords are valid");
    expectTrue(!fuse::renderer::ProbeGridLayout::sampleCoordsTouchBorder(desc, built),
               "interior sample coords do not touch border");

    fuse::renderer::DDGIDesc borderDesc = desc;
    borderDesc.grid_dims = {2, 2, 2};
    fuse::renderer::ProbeSampleCoords border{};
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(borderDesc, {0.f, 0.f, 0.f}, border),
               "tryBuildProbeSampleCoords succeeds for border sample");
    expectTrue(fuse::renderer::ProbeGridLayout::sampleCoordsTouchBorder(borderDesc, border),
               "border sample coords touch grid border");

    fuse::renderer::ProbeSampleCoords invalid{};
    invalid.x0 = 2u;
    invalid.x1 = 1u;
    invalid.y0 = 0u;
    invalid.y1 = 1u;
    invalid.z0 = 0u;
    invalid.z1 = 1u;
    invalid.tx = 0.5f;
    invalid.ty = 0.5f;
    invalid.tz = 0.5f;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalid),
               "inverted x indices fail validation");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 1.5f;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, oobWeights),
               "OOB trilinear weight fails validation");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::renderer::ProbeSampleCoords emptyBuilt{};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(empty, {0.f, 0.f, 0.f}, emptyBuilt),
               "tryBuildProbeSampleCoords fails on empty grid");
}

void testCacheSizingGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::requiredCacheCount(desc) == 8u,
               "required cache count matches probe count");
    expectTrue(fuse::renderer::ddgi_util::cacheDeficitForGrid(desc, 8u) == 0u,
               "full cache has zero deficit");
    expectTrue(fuse::renderer::ddgi_util::cacheDeficitForGrid(desc, 5u) == 3u,
               "undersized cache reports deficit");
    expectTrue(fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 8u),
               "full cache passes sizing guard");
    expectTrue(!fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 5u),
               "undersized cache fails sizing guard");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::requiredCacheCount(empty) == 0u,
               "empty grid requires zero cache entries");
    expectTrue(fuse::renderer::ddgi_util::cacheDeficitForGrid(empty, 0u) == 0u,
               "empty grid cache deficit is zero");
}

void testProbeSampleSkipClassification() {
    using fuse::renderer::ProbeSampleSkipReason;

    expectTrue(!fuse::renderer::probeSampleSkipReasonIsBlocking(ProbeSampleSkipReason::None),
               "None skip reason is not blocking");
    expectTrue(fuse::renderer::probeSampleSkipReasonIsBlocking(ProbeSampleSkipReason::UndersizedCache),
               "UndersizedCache skip reason is blocking");
    expectTrue(std::string(fuse::renderer::probeSampleSkipReasonLabel(ProbeSampleSkipReason::EmptyGrid)) ==
                   "empty_grid",
               "EmptyGrid skip reason label");

    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSkip(desc) == ProbeSampleSkipReason::None,
               "valid grid passes grid skip classification");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeSampleSkip(desc, 8u) == ProbeSampleSkipReason::None,
               "valid grid and cache pass sample skip classification");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeSampleSkip(desc, 4u) ==
                   ProbeSampleSkipReason::UndersizedCache,
               "undersized cache classified as UndersizedCache");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSkip(empty) == ProbeSampleSkipReason::EmptyGrid,
               "empty grid classified as EmptyGrid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSkip(zeroRes) ==
                   ProbeSampleSkipReason::ZeroIrradianceResolution,
               "zero irradiance_res classified");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 1.f, 1.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSkip(badSpacing) ==
                   ProbeSampleSkipReason::InvalidProbeSpacing,
               "invalid spacing classified");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeSampleLookup(desc, cache.data(), 8u) ==
                   ProbeSampleSkipReason::None,
               "valid lookup passes classification");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeSampleLookup(desc, nullptr, 8u) ==
                   ProbeSampleSkipReason::NullCache,
               "null cache classified as NullCache");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeSampleLookup(empty, cache.data(), 8u) ==
                   ProbeSampleSkipReason::EmptyGrid,
               "empty grid lookup prefers EmptyGrid over NullCache");
}

void testResolveSampleDirectionFromSurface() {
    const fuse::math::Vec3 up{0.f, 1.f, 0.f};
    const fuse::math::Vec3 right{1.f, 0.f, 0.f};

    const fuse::math::Vec3 fromDirection =
        fuse::renderer::DdgiIrradianceEncoding::resolveSampleDirectionFromSurface(right, up);
    expectNear(fromDirection.x, 1.f, 1e-5f, "surface resolve prefers explicit direction");

    const fuse::math::Vec3 fromNormal =
        fuse::renderer::DdgiIrradianceEncoding::resolveSampleDirectionFromSurface({0.f, 0.f, 0.f}, up);
    expectNear(fromNormal.y, 1.f, 1e-5f, "surface resolve falls back to surface normal");

    const fuse::math::Vec3 fromDefault =
        fuse::renderer::DdgiIrradianceEncoding::resolveSampleDirectionFromSurface({0.f, 0.f, 0.f},
                                                                                {0.f, 0.f, 0.f});
    expectNear(fromDefault.y, 1.f, 1e-5f, "surface resolve falls back to +Y when both empty");

    expectTrue(fuse::renderer::DdgiIrradianceEncoding::isValidDirection(up),
               "isValidDirection true for unit vector");
    expectTrue(!fuse::renderer::DdgiIrradianceEncoding::isValidDirection({0.f, 0.f, 0.f}),
               "isValidDirection false for zero vector");
    expectTrue(fuse::renderer::DdgiIrradianceEncoding::canDirectionallySample(8u),
               "canDirectionallySample true for non-zero tile res");
    expectTrue(!fuse::renderer::DdgiIrradianceEncoding::canDirectionallySample(0u),
               "canDirectionallySample false for zero tile res");
}

void testEmptyDirectionGuards() {
    expectTrue(!fuse::renderer::DdgiIrradianceEncoding::isEmptyDirection({0.f, 1.f, 0.f}),
               "unit +Y is not empty");

    const fuse::math::Vec3 fallback =
        fuse::renderer::DdgiIrradianceEncoding::resolveSampleDirection({0.f, 0.f, 0.f});
    expectNear(fallback.y, 1.f, 1e-5f, "empty direction resolves to +Y fallback");

    const fuse::math::Vec3 customFallback =
        fuse::renderer::DdgiIrradianceEncoding::resolveSampleDirection({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f});
    expectNear(customFallback.x, 1.f, 1e-5f, "empty direction uses custom fallback axis");

    const fuse::math::Vec2 encodedEmpty =
        fuse::renderer::DdgiIrradianceEncoding::encodeDirection({0.f, 0.f, 0.f});
    const fuse::math::Vec2 encodedUp =
        fuse::renderer::DdgiIrradianceEncoding::encodeDirection({0.f, 1.f, 0.f});
    expectNear(encodedEmpty.x, encodedUp.x, 1e-5f, "empty direction encodes like +Y fallback");
    expectNear(encodedEmpty.y, encodedUp.y, 1e-5f, "empty direction encodes like +Y fallback v");

    fuse::renderer::IrradianceCacheEntry entry{};
    entry.irradiance = {1.f, 0.5f, 0.25f};
    const fuse::math::Vec3 emptySample =
        fuse::renderer::ddgi_util::sampleDirectionalIrradianceAtProbe(entry, {0.f, 0.f, 0.f}, 8u);
    const fuse::math::Vec3 upSample =
        fuse::renderer::ddgi_util::sampleDirectionalIrradianceAtProbe(entry, {0.f, 1.f, 0.f}, 8u);
    expectTrue(emptySample.x > 0.f, "empty direction sample falls back to non-zero irradiance");
    expectNear(emptySample.x, upSample.x, 1e-4f, "empty direction sample matches +Y sample");

    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;
    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};
    }
    const fuse::math::Vec3 emptyTrilinear =
        fuse::renderer::ddgi_util::trilinearDirectionalProbeIrradiance(
            desc, {0.5f, 0.5f, 0.5f}, {0.f, 0.f, 0.f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    const fuse::math::Vec3 upTrilinear =
        fuse::renderer::ddgi_util::trilinearDirectionalProbeIrradiance(
            desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    expectTrue(emptyTrilinear.x > 0.f, "empty direction trilinear sample is non-zero");
    expectNear(emptyTrilinear.x, upTrilinear.x, 1e-4f, "empty direction trilinear matches +Y");
}

void testProbeWorldPositionClamped() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {1.f, 2.f, 3.f};
    desc.probe_spacing = {2.f, 2.f, 2.f};
    desc.grid_dims = {2, 2, 2};

    const fuse::math::Vec3 last =
        fuse::renderer::ddgi_util::probeWorldPositionClamped(desc, UINT32_MAX);
    const fuse::math::Vec3 expected =
        fuse::renderer::ddgi_util::probeWorldPosition(desc, 7u);
    expectNear(last.x, expected.x, 1e-5f, "clamped world position x matches last probe");
    expectNear(last.y, expected.y, 1e-5f, "clamped world position y matches last probe");
    expectNear(last.z, expected.z, 1e-5f, "clamped world position z matches last probe");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_origin = {4.f, 5.f, 6.f};
    empty.grid_dims = {0, 2, 2};
    const fuse::math::Vec3 emptyPos =
        fuse::renderer::ddgi_util::probeWorldPosition(empty, 99u);
    expectNear(emptyPos.x, 4.f, 1e-5f, "empty grid probeWorldPosition returns origin");
}

void testIrradianceOctahedralEncoding() {
    const fuse::math::Vec3 up{0.f, 1.f, 0.f};
    const fuse::math::Vec2 encoded = fuse::renderer::DdgiIrradianceEncoding::encodeDirection(up);
    const fuse::math::Vec3 decoded = fuse::renderer::DdgiIrradianceEncoding::decodeDirection(encoded);
    const fuse::f32 error = fuse::renderer::DdgiIrradianceEncoding::angularErrorRadians(up, decoded);
    expectTrue(error < 0.001f, "octahedral direction round-trip < 0.001 rad");

    const fuse::math::Vec2 zeroDirEncoded =
        fuse::renderer::DdgiIrradianceEncoding::encodeDirection({0.f, 0.f, 0.f});
    const fuse::math::Vec2 upDirEncoded =
        fuse::renderer::DdgiIrradianceEncoding::encodeDirection({0.f, 1.f, 0.f});
    expectNear(zeroDirEncoded.x, upDirEncoded.x, 1e-5f, "zero direction encodes like +Y fallback u");
    expectNear(zeroDirEncoded.y, upDirEncoded.y, 1e-5f, "zero direction encodes like +Y fallback v");

    const fuse::math::Vec2 oobEncoded{1.5f, -0.25f};
    const fuse::math::Vec2 clampedEncoded =
        fuse::renderer::DdgiIrradianceEncoding::clampEncodedUV(oobEncoded);
    expectNear(clampedEncoded.x, 1.f, 1e-5f, "clampEncodedUV clamps high u");
    expectNear(clampedEncoded.y, 0.f, 1e-5f, "clampEncodedUV clamps low v");
    const fuse::math::Vec3 decodedOob =
        fuse::renderer::DdgiIrradianceEncoding::decodeDirection(oobEncoded);
    const fuse::math::Vec3 decodedClamped =
        fuse::renderer::DdgiIrradianceEncoding::decodeDirection(clampedEncoded);
    expectNear(decodedOob.x, decodedClamped.x, 1e-5f, "decode clamps OOB encoded u");
    expectNear(decodedOob.y, decodedClamped.y, 1e-5f, "decode clamps OOB encoded v");

    expectNear(fuse::renderer::DdgiIrradianceEncoding::angularErrorRadians({0.f, 0.f, 0.f}, up), 0.f, 1e-5f,
               "angular error zero for degenerate direction");

    fuse::renderer::DdgiTileBilinearCoords tileCoords{};
    expectTrue(fuse::renderer::DdgiIrradianceEncoding::buildTileBilinearCoords(up, 8u, tileCoords),
               "buildTileBilinearCoords succeeds for +Y");
    expectTrue(tileCoords.texel_u1 >= tileCoords.texel_u0 && tileCoords.texel_v1 >= tileCoords.texel_v0,
               "tile bilinear texel indices are ordered");
    expectTrue(tileCoords.tu >= 0.f && tileCoords.tu <= 1.f && tileCoords.tv >= 0.f && tileCoords.tv <= 1.f,
               "tile bilinear fractions stay in unit range");

    fuse::renderer::DdgiTileBilinearCoords singleTexel{};
    expectTrue(fuse::renderer::DdgiIrradianceEncoding::buildTileBilinearCoords(up, 1u, singleTexel),
               "buildTileBilinearCoords succeeds for 1x1 tile");
    expectTrue(singleTexel.texel_u0 == 0u && singleTexel.texel_u1 == 0u, "1x1 tile u indices collapse");
    expectTrue(singleTexel.texel_v0 == 0u && singleTexel.texel_v1 == 0u, "1x1 tile v indices collapse");
    expectTrue(!fuse::renderer::DdgiIrradianceEncoding::buildTileBilinearCoords(up, 0u, singleTexel),
               "buildTileBilinearCoords fails for zero irradiance_res");

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

    fuse::renderer::DDGIDesc emptyDesc{};
    emptyDesc.grid_dims = {0, 2, 2};
    const fuse::math::Vec2 emptyOrigin =
        fuse::renderer::ProbeGridLayout::probeIrradianceAtlasOrigin(emptyDesc, coord);
    expectNear(emptyOrigin.x, 0.f, 1e-5f, "empty grid irradiance atlas origin is zero");
    const fuse::renderer::ProbeGridCoord invalid{9, 0, 0};
    const fuse::math::Vec2 invalidOrigin =
        fuse::renderer::ProbeGridLayout::probeIrradianceAtlasOrigin(desc, invalid);
    expectNear(invalidOrigin.x, 0.f, 1e-5f, "invalid coord irradiance atlas origin is zero");
}

void testDirectionalProbeIrradiance() {
    fuse::renderer::IrradianceCacheEntry entry{};
    entry.irradiance = {1.f, 0.5f, 0.25f};

    const fuse::math::Vec3 upSample =
        fuse::renderer::ddgi_util::sampleDirectionalIrradianceAtProbe(entry, {0.f, 1.f, 0.f}, 8u);
    expectTrue(upSample.x > 0.f, "directional sample toward +Y is non-zero");

    const fuse::math::Vec3 downSample =
        fuse::renderer::ddgi_util::sampleDirectionalIrradianceAtProbe(entry, {0.f, -1.f, 0.f}, 8u);
    expectTrue(downSample.x <= upSample.x, "directional sample away from +Y is weaker");

    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};
    }

    const fuse::math::Vec3 directional =
        fuse::renderer::ddgi_util::trilinearDirectionalProbeIrradiance(
            desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), static_cast<fuse::u32>(cache.size()));
    expectTrue(directional.x > 0.f, "trilinear directional sample is non-zero at grid centre");
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

    const fuse::math::Vec3 identical = fuse::renderer::ddgi_util::lerpIrradiance(a, a, 0.75f);
    expectNear(identical.x, 1.f, 1e-5f, "lerp identical endpoints returns a");

    const fuse::math::Vec3 fullRetain =
        fuse::renderer::ddgi_util::blendIrradiance(a, b, 1.5f);
    expectNear(fullRetain.x, 1.f, 1e-5f, "hysteresis >1 retains previous");
    const fuse::math::Vec3 fullReplace =
        fuse::renderer::ddgi_util::blendIrradiance(a, b, -0.5f);
    expectNear(fullReplace.y, 1.f, 1e-5f, "hysteresis <0 replaces with incoming");
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

    const fuse::math::Vec3 oobUv =
        fuse::renderer::ddgi_util::bilinearTileIrradiance(samples, 2.f, -1.f);
    const fuse::math::Vec3 clampedEdge =
        fuse::renderer::ddgi_util::bilinearTileIrradiance(samples, 1.f, 0.f);
    expectNear(oobUv.x, clampedEdge.x, 1e-5f, "bilinear OOB uv clamps to edge sample");
    const fuse::math::Vec3 nullSample =
        fuse::renderer::ddgi_util::bilinearTileIrradiance(nullptr, 0.5f, 0.5f);
    expectNear(nullSample.x, 0.f, 1e-5f, "bilinear null samples returns zero");
}

void testProbeIndexBoundsHelpers() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(desc) == 7u,
               "maxProbeIndex returns last probe index");
    expectTrue(fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(7u, desc),
               "last probe index flagged at max");
    expectTrue(!fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(6u, desc),
               "non-last probe index not at max");

    fuse::u32 clamped = 99u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeIndex(3u, desc, clamped),
               "tryClampProbeIndex succeeds on non-empty grid");
    expectTrue(clamped == 3u, "tryClampProbeIndex preserves in-range index");

    fuse::u32 oobClamped = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeIndex(99u, desc, oobClamped),
               "tryClampProbeIndex succeeds for OOB index");
    expectTrue(oobClamped == 7u, "tryClampProbeIndex clamps OOB index to max");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::u32 emptyClamped = 5u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeIndex(5u, empty, emptyClamped),
               "tryClampProbeIndex fails on empty grid");
    expectTrue(emptyClamped == 0u, "tryClampProbeIndex zeroes out_index on empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(empty) == 0u,
               "maxProbeIndex zero on empty grid");
void testProbeSampleCoordDeepenGuards() {

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "buildProbeSampleCoords produces coords for interior sample");
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, built),
               "built sample coords pass in-bounds guard");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, built),
               "built sample coords are not out of range");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, oobIndices),
               "OOB corner indices flagged out of range");
    expectTrue(!fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, oobIndices),
               "OOB corner indices fail in-bounds guard");

    fuse::renderer::ProbeSampleCoords extremeWeights = built;
    extremeWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, extremeWeights),
               "OOB trilinear weights flagged out of range");

    fuse::renderer::ProbeSampleCoords unclamped = oobIndices;
    const fuse::u32 originalX0 = unclamped.x0;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(empty, unclamped),
               "tryClampProbeSampleCoords rejects empty grid");
    expectTrue(unclamped.x0 == originalX0, "tryClampProbeSampleCoords leaves coords unchanged on empty grid");

    fuse::renderer::ProbeSampleCoords inBounds{};
    inBounds.x0 = 0u;
    inBounds.y0 = 0u;
    inBounds.z0 = 0u;
    inBounds.x1 = 1u;
    inBounds.y1 = 1u;
    inBounds.z1 = 1u;
    inBounds.tx = 0.25f;
    inBounds.ty = 0.5f;
    inBounds.tz = 0.75f;
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, inBounds),
               "in-bounds probe sample coords pass bounds check");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, inBounds),
               "in-bounds probe sample coords are not out of range");

    fuse::renderer::ProbeSampleCoords outOfRange = inBounds;
    outOfRange.x0 = 99u;
    expectTrue(!fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, outOfRange),
               "OOB probe corner fails bounds check");
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, outOfRange),
               "OOB probe corner is out of range");

    fuse::renderer::ProbeSampleCoords extremeWeights = inBounds;
               "OOB interpolation weight is out of range");

    fuse::renderer::ProbeSampleCoords unclamped = outOfRange;
    const fuse::u32 originalX = unclamped.x0;
    expectTrue(unclamped.x0 == originalX, "tryClampProbeSampleCoords leaves coords unchanged on empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, unclamped),
               "tryClampProbeSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, unclamped),
               "tryClampProbeSampleCoords produces in-bounds coords");

    fuse::renderer::ProbeSampleRejectReason buildReason = fuse::renderer::ProbeSampleRejectReason::None;
    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built,
                                                                          buildReason),
               "tryBuildProbeSampleCoords succeeds on interior sample");
    expectTrue(buildReason == fuse::renderer::ProbeSampleRejectReason::None,
               "successful build reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleRejectReasonLabel(buildReason), "none") == 0,
               "none probe sample reject reason label");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(empty, {0.f, 0.f, 0.f}, built,
               "tryBuildProbeSampleCoords rejects empty grid");
    expectTrue(buildReason == fuse::renderer::ProbeSampleRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleRejectReasonLabel(buildReason), "empty_grid") == 0,
               "empty_grid probe sample reject reason label");

    expectTrue(fuse::renderer::ddgi_util::canSampleAtProbeCoords(desc, inBounds, 8u),
               "canSampleAtProbeCoords accepts valid coords with sized cache");
    fuse::renderer::ProbeSampleRejectReason sampleReason = fuse::renderer::ProbeSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, inBounds, 8u, sampleReason),
               "tryCanSampleAtProbeCoords succeeds on accessible grid");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleRejectReason::None,
               "accessible grid reports no sample reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, outOfRange, 8u, sampleReason),
               "tryCanSampleAtProbeCoords rejects OOB coords");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleRejectReason::OutOfBounds,
               "OOB coords report out_of_bounds sample reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleRejectReasonLabel(sampleReason), "out_of_bounds") == 0,
               "out_of_bounds probe sample reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(empty, inBounds, 8u, sampleReason),
               "tryCanSampleAtProbeCoords rejects empty grid");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample reject reason via cache preflight");
}

void testCacheIndexDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    expectTrue(!fuse::renderer::ddgi_util::shouldSkipProbeSample(desc, 8u),
               "sized cache does not skip probe sample");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipProbeUpdate(desc),
               "non-empty grid does not skip probe update");
    expectTrue(fuse::renderer::ddgi_util::canLookupCacheAtIndex(desc, 0u, 8u),
               "canLookupCacheAtIndex accepts origin probe in full cache");

    fuse::renderer::CacheLookupRejectReason lookupReason = fuse::renderer::CacheLookupRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanLookupCacheAtIndex(desc, 5u, 8u, lookupReason),
               "tryCanLookupCacheAtIndex succeeds on accessible cache");
    expectTrue(lookupReason == fuse::renderer::CacheLookupRejectReason::None,
               "accessible cache reports no lookup reject reason");
    expectTrue(std::strcmp(fuse::renderer::cacheLookupRejectReasonLabel(lookupReason), "none") == 0,
               "none cache lookup reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtIndex(desc, 8u, 8u, lookupReason),
               "probe index equal to cache length fails lookup preflight");
    expectTrue(lookupReason == fuse::renderer::CacheLookupRejectReason::ProbeIndexOutOfRange,
               "equal probe index reports probe_index_out_of_range reject reason");
    expectTrue(std::strcmp(fuse::renderer::cacheLookupRejectReasonLabel(lookupReason),
                           "probe_index_out_of_range") == 0,
               "probe_index_out_of_range cache lookup reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtIndex(desc, 3u, 2u, lookupReason),
               "undersized cache fails lookup preflight");
    expectTrue(lookupReason == fuse::renderer::CacheLookupRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache reject reason");
    expectTrue(std::strcmp(fuse::renderer::cacheLookupRejectReasonLabel(lookupReason), "undersized_cache") == 0,
               "undersized_cache cache lookup reject reason label");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeSample(empty, 8u),
               "empty grid skips probe sample");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeUpdate(empty),
               "empty grid skips probe update");
    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtIndex(empty, 0u, 8u, lookupReason),
               "empty grid fails cache lookup preflight");
    expectTrue(lookupReason == fuse::renderer::CacheLookupRejectReason::EmptyGrid,
               "empty grid reports empty_grid cache reject reason");

    fuse::renderer::DDGISampleRequest request{};
    request.world_position = {0.5f, 0.5f, 0.5f};
    expectTrue(fuse::renderer::ddgi_util::tryIsValidSampleRequest(desc, request, 8u, lookupReason),
               "tryIsValidSampleRequest succeeds with sized cache");
               "valid sample request reports no cache reject reason");
    expectTrue(!fuse::renderer::ddgi_util::tryIsValidSampleRequest(desc, request, 4u, lookupReason),
               "tryIsValidSampleRequest rejects undersized cache");
               "undersized cache sample request reports undersized_cache reject reason");

void testLaunchAndKernelPreflightGuards() {

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::LaunchRejectReason launchReason = fuse::renderer::LaunchRejectReason::None;
    expectTrue(fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, launchReason),
               "tryCanLaunch succeeds on valid indices");
    expectTrue(launchReason == fuse::renderer::LaunchRejectReason::None,
               "valid launch reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::launchRejectReasonLabel(launchReason), "none") == 0,
               "none launch reject reason label");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, oobIndices, 2u, launchReason),
               "tryCanLaunch rejects OOB probe indices");
    expectTrue(launchReason == fuse::renderer::LaunchRejectReason::ProbeIndexOutOfRange,
               "OOB indices report probe_index_out_of_range launch reject reason");
    expectTrue(std::strcmp(fuse::renderer::launchRejectReasonLabel(launchReason), "probe_index_out_of_range") ==
                   0,
               "probe_index_out_of_range launch reject reason label");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, nullptr, 1u, launchReason),
               "tryCanLaunch rejects null index buffer");
    expectTrue(launchReason == fuse::renderer::LaunchRejectReason::NullIndices,
               "null indices report null_indices launch reject reason");
    expectTrue(std::strcmp(fuse::renderer::launchRejectReasonLabel(launchReason), "null_indices") == 0,
               "null_indices launch reject reason label");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 0u, launchReason),
               "tryCanLaunch rejects zero probe count");
    expectTrue(launchReason == fuse::renderer::LaunchRejectReason::ZeroCount,
               "zero count reports zero_count launch reject reason");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(zeroRays, validIndices, 2u, launchReason),
               "tryCanLaunch rejects zero rays_per_probe");
    expectTrue(launchReason == fuse::renderer::LaunchRejectReason::ZeroRaysPerProbe,
               "zero rays report zero_rays_per_probe launch reject reason");
    expectTrue(std::strcmp(fuse::renderer::launchRejectReasonLabel(launchReason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe launch reject reason label");
    expectTrue(fuse::renderer::canLaunchDdgiProbeUpdate(zeroRays, validIndices, 2u),
               "canLaunchDdgiProbeUpdate unchanged for zero rays_per_probe");

    fuse::renderer::gi::DDGIKernelParams traceParams{};
    traceParams.probe_indices_to_update = validIndices;
    traceParams.probe_update_count = 2u;
    traceParams.rays_per_probe = 256u;
    fuse::renderer::gi::KernelLaunchRejectReason kernelReason =
        fuse::renderer::gi::KernelLaunchRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(traceParams, kernelReason),
               "tryCanLaunchProbeTraceKernel succeeds on valid params");
    expectTrue(kernelReason == fuse::renderer::gi::KernelLaunchRejectReason::None,
               "valid trace kernel params report no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::kernelLaunchRejectReasonLabel(kernelReason), "none") == 0,
               "none kernel launch reject reason label");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(traceParams, nullptr),
               "launch_probe_trace_kernel succeeds on valid params");

    fuse::renderer::gi::DDGIKernelParams invalidTrace = traceParams;
    invalidTrace.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(invalidTrace, kernelReason),
               "tryCanLaunchProbeTraceKernel rejects zero rays");
    expectTrue(kernelReason == fuse::renderer::gi::KernelLaunchRejectReason::ZeroRaysPerProbe,
               "zero rays report zero_rays_per_probe kernel reject reason");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(invalidTrace, nullptr),
               "launch_probe_trace_kernel rejects zero rays");

    fuse::renderer::gi::DDGIKernelParams nullIndices = traceParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(nullIndices, kernelReason),
               "tryCanLaunchProbeTraceKernel rejects null indices");
    expectTrue(kernelReason == fuse::renderer::gi::KernelLaunchRejectReason::NullIndices,
               "null indices report null_indices kernel reject reason");

    fuse::renderer::gi::DDGIKernelParams zeroCount = traceParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(zeroCount, kernelReason),
               "tryCanLaunchProbeBlendKernel rejects zero count");
    expectTrue(kernelReason == fuse::renderer::gi::KernelLaunchRejectReason::ZeroCount,
               "zero count reports zero_count kernel reject reason");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(zeroCount, nullptr),
               "launch_probe_blend_kernel rejects zero count");

    expectTrue(fuse::renderer::gi::canLaunchProbeBlendKernel(traceParams),
               "canLaunchProbeBlendKernel accepts valid params");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(traceParams, nullptr),
               "launch_probe_blend_kernel succeeds on valid params");
}

void testProbeSampleCoordGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "buildProbeSampleCoords produces coords for interior sample");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, built),
               "built sample coords pass validity guard");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "valid sample coords do not skip preflight");

    fuse::renderer::ProbeSampleCoords tryBuilt{};
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, tryBuilt),
               "tryBuildProbeSampleCoords succeeds for interior sample");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, tryBuilt),
               "tryBuild sample coords pass validity guard");
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),

    fuse::renderer::ProbeSampleCoords reversed{};
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    reversed.y0 = 1u;
    reversed.y1 = 0u;
    reversed.z0 = 1u;
    reversed.z1 = 0u;
    reversed.tx = 0.25f;
    reversed.ty = 0.75f;
    reversed.tz = 0.5f;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, reversed),
               "reversed corner indices fail validity guard");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "reversed corners skip sample-coord preflight");
    fuse::renderer::ProbeGridLayout::normalizeProbeSampleCoords(reversed);
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, reversed),
               "normalizeProbeSampleCoords fixes reversed corners");
    expectTrue(reversed.x0 == 0u && reversed.x1 == 1u, "normalize swaps x corners into order");
    expectNear(reversed.tx, 0.75f, 1e-5f, "normalize inverts tx when x corners swap");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    oobWeights.ty = -1.f;
    fuse::renderer::ProbeGridLayout::normalizeProbeSampleCoords(oobWeights);
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, oobWeights),
               "normalize clamps OOB trilinear weights");

    fuse::renderer::ProbeSampleCoords oobIndices{};
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    oobIndices.y0 = 9u;
    oobIndices.y1 = 9u;
    oobIndices.z0 = 9u;
    oobIndices.z1 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, oobIndices),
               "OOB corner indices fail validity guard before clamp");
    fuse::renderer::ProbeGridLayout::clampProbeSampleCoords(desc, oobIndices);
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, oobIndices),
               "clampProbeSampleCoords yields valid sample coords");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(empty, built),
               "empty grid sample coords invalid");
    fuse::renderer::ProbeSampleCoords emptyBuilt{};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(empty, {0.f, 0.f, 0.f}, emptyBuilt),
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(empty, {0.5f, 0.5f, 0.5f}, built),
               "tryBuildProbeSampleCoords fails on empty grid");
}

void testProbeSampleCoordBoundsGuards() {
void testCacheIndexDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 0u, 8u, reason),
               "valid cache index passes tryIsCacheIndexValid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None, "valid cache index reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "none") == 0,
               "cache index reject label for none");

    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexOutOfRange(desc, 0u, 8u),
               "valid cache index is not out of range");
    expectTrue(fuse::renderer::ddgi_util::isCacheIndexOutOfRange(desc, 8u, 8u),
               "index equal to cache length is out of range");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 99u, 8u, reason),
               "OOB probe index rejected by tryIsCacheIndexValid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeIndex,
               "OOB probe index reports out_of_range_index");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "out_of_range_index") == 0,
               "cache index reject label for out_of_range_index");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 3u, 2u, reason),
               "undersized cache rejected by tryIsCacheIndexValid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(empty, 0u, 8u, reason),
               "empty grid rejected by tryIsCacheIndexValid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid reports empty_grid");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "empty_grid") == 0,
               "cache index reject label for empty_grid");


    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "buildProbeSampleCoords produces coords for bounds guard test");
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, built),
               "built sample coords pass in-bounds guard");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, built),
               "built sample coords are not out of range");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, reversed),
               "reversed corners still pass in-bounds guard");
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, reversed),
               "reversed corners fail strict validity guard");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, oobWeights),
               "OOB trilinear weight fails in-bounds guard");
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, oobWeights),
               "OOB trilinear weight flagged out of range");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, oobIndices),
               "OOB corner indices flagged out of range");

    fuse::renderer::ProbeSampleCoords unclamped = oobIndices;
    const fuse::u32 originalX0 = unclamped.x0;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(empty, unclamped),
               "tryClampProbeSampleCoords rejects empty grid");
    expectTrue(unclamped.x0 == originalX0, "tryClampProbeSampleCoords leaves coords unchanged on empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, unclamped),
               "tryClampProbeSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, unclamped),
               "tryClampProbeSampleCoords produces in-bounds coords");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(empty, built),
               "empty grid skips sample-coord preflight");
}

void testCacheIndexGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "buildProbeSampleCoords for bounds test");
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, built),
               "built sample coords are in bounds");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, built),
               "built sample coords not out of range");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, oobIndices),
               "OOB corner indices fail in-bounds check");
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, oobIndices),
               "OOB corner indices flagged out of range");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, oobWeights),
               "OOB trilinear weight flagged out of range");

    fuse::renderer::ProbeSampleCoords toClamp = oobIndices;
    const fuse::u32 originalX0 = toClamp.x0;
    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(empty, toClamp),
               "tryClampProbeSampleCoords rejects empty grid");
    expectTrue(toClamp.x0 == originalX0, "tryClampProbeSampleCoords leaves coords unchanged on empty grid");

    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, toClamp),
               "tryClampProbeSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, toClamp),
               "tryClampProbeSampleCoords yields in-bounds coords");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, toClamp),
               "tryClampProbeSampleCoords yields valid sample coords");
}

void testCacheIndexGuards() {

    expectTrue(fuse::renderer::ddgi_util::requiredCacheCount(desc) == 8u,
               "requiredCacheCount matches probeCount");
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

               "requiredCacheCount matches probe count for sampleable grid");
    expectTrue(fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 8u),
               "cache sized at required count");
    expectTrue(!fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 7u),
               "cache one short of required count rejected");

    expectTrue(fuse::renderer::ddgi_util::isCacheIndexValid(desc, 0u, 8u),
               "origin probe index valid in full cache");
    expectTrue(fuse::renderer::ddgi_util::isCacheIndexValid(desc, 7u, 8u),
               "last probe index valid in full cache");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValid(desc, 8u, 8u),
               "probe index equal to cache length rejected");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValid(desc, 99u, 8u),
               "OOB probe index rejected even with full cache");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValid(desc, 3u, 2u),
               "in-range probe index rejected when cache undersized");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::requiredCacheCount(empty) == 0u,
               "requiredCacheCount zero on empty grid");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValid(empty, 0u, 8u),
               "cache index invalid on empty grid");
    expectTrue(fuse::renderer::ddgi_util::isCacheIndexOutOfRange(empty, 0u, 8u),
               "cache index out of range on empty grid");
    expectTrue(fuse::renderer::ddgi_util::isCacheIndexOutOfRange(desc, 3u, 2u),
               "in-range probe index out of range when cache undersized");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexOutOfRange(desc, 3u, 8u),
               "in-range probe index not out of range in full cache");

    fuse::u32 cacheClamped = 0u;
    expectTrue(fuse::renderer::ddgi_util::tryClampCacheIndex(desc, 99u, 8u, cacheClamped),
               "tryClampCacheIndex succeeds on non-empty grid");
    expectTrue(cacheClamped == 7u, "tryClampCacheIndex clamps OOB probe index");

    fuse::u32 undersizedClamped = 0u;
    expectTrue(fuse::renderer::ddgi_util::tryClampCacheIndex(desc, 5u, 2u, undersizedClamped),
               "tryClampCacheIndex succeeds with undersized cache");
    expectTrue(undersizedClamped == 1u, "tryClampCacheIndex clamps to last cache entry");

    fuse::u32 emptyClamped = 4u;
    expectTrue(!fuse::renderer::ddgi_util::tryClampCacheIndex(empty, 0u, 8u, emptyClamped),
               "tryClampCacheIndex fails on empty grid");
    expectTrue(emptyClamped == 0u, "tryClampCacheIndex zeroes out_index on empty grid");
}

void testLaunchProbeUpdateDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::DdgiLaunchRejectReason reason = fuse::renderer::DdgiLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "tryCanLaunchDdgiProbeUpdate accepts in-range indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::None, "valid launch reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "none") == 0,
               "launch reject label for none");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, nullptr, 1u, reason),
               "tryCanLaunchDdgiProbeUpdate rejects null indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::NullIndices,
               "null indices report null_indices");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "null_indices") == 0,
               "launch reject label for null_indices");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 0u, reason),
               "tryCanLaunchDdgiProbeUpdate rejects zero count");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::ZeroCount,
               "zero count reports zero_count");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, oobIndices, 2u, reason),
               "tryCanLaunchDdgiProbeUpdate rejects OOB indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::OutOfRangeIndex,
               "OOB indices report out_of_range_index");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(empty, validIndices, 2u, reason),
               "tryCanLaunchDdgiProbeUpdate rejects empty grid");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::EmptyGrid,
               "empty grid reports empty_grid");

void testKernelLaunchPreflightGuards() {
    desc.rays_per_probe = 256u;

    fuse::u32 indices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams params{};
    params.probe_indices_to_update = indices;
    params.probe_update_count = 2u;
    params.rays_per_probe = desc.rays_per_probe;

    fuse::renderer::gi::DdgiKernelRejectReason reason = fuse::renderer::gi::DdgiKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::canLaunchProbeTraceKernel(params),
               "trace kernel preflight accepts valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeBlendKernel(params),
               "blend kernel preflight accepts valid params");
    expectTrue(fuse::renderer::gi::canLaunchDdgiKernels(desc, params),
               "combined kernel preflight accepts valid params");
    expectTrue(fuse::renderer::gi::tryCanLaunchDdgiKernels(desc, params, reason),
               "tryCanLaunchDdgiKernels accepts valid params");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::None,
               "valid kernel launch reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "none") == 0,
               "kernel reject label for none");

    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "trace kernel launch succeeds with valid params");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(params, nullptr),
               "blend kernel launch succeeds with valid params");

    fuse::renderer::gi::DDGIKernelParams nullParams = params;
    nullParams.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(nullParams, reason),
               "trace kernel preflight rejects null indices");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::NullIndices,
               "null indices report null_indices for trace kernel");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(nullParams, nullptr),
               "trace kernel launch rejects null indices");

    fuse::renderer::gi::DDGIKernelParams zeroCount = params;
    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(zeroCount, reason),
               "blend kernel preflight rejects zero count");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroCount,
               "zero count reports zero_count for blend kernel");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(zeroCount, nullptr),
               "blend kernel launch rejects zero count");

    fuse::renderer::gi::DDGIKernelParams zeroRays = params;
    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(zeroRays, reason),
               "trace kernel preflight rejects zero rays_per_probe");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::InvalidRaysPerProbe,
               "zero rays_per_probe reports invalid_rays_per_probe");

    fuse::renderer::gi::DDGIKernelParams oobParams = params;
    oobParams.probe_indices_to_update = oobIndices;
    expectTrue(!fuse::renderer::gi::tryCanLaunchDdgiKernels(desc, oobParams, reason),
               "combined kernel preflight rejects OOB indices");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::OutOfRangeIndex,
               "OOB indices report out_of_range_index for kernel launch");
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(desc, oobIndices, 2u, nullptr),
               "host launch rejects OOB indices via combined kernel preflight");

void testProbeSampleCoordsRejectReasons() {

    fuse::renderer::ProbeSampleCoords built{};
    fuse::renderer::ProbeSampleCoordsRejectReason reason = fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built, reason),
               "tryBuildProbeSampleCoords succeeds on interior sample");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None, "interior build reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "none") == 0,
               "none sample-coords reject label");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(empty, {0.f, 0.f, 0.f}, built, reason),
               "tryBuildProbeSampleCoords rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid build reports EmptyGrid");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid sample-coords reject label");

    fuse::renderer::ProbeSampleCoords oobIndices{};
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    oobIndices.y0 = 9u;
    oobIndices.y1 = 9u;
    oobIndices.z0 = 9u;
    oobIndices.z1 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, oobIndices, reason),
               "tryValidate rejects OOB corner indices");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "OOB indices report OutOfRangeIndices");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "out_of_range_indices") == 0,
               "out_of_range_indices sample-coords reject label");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, reversed, reason),
               "tryValidate rejects unordered corners");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "unordered corners report UnorderedCorners");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, oobWeights, reason),
               "tryValidate rejects OOB trilinear weights");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "OOB weights report OutOfRangeWeights");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "out_of_range_weights") == 0,
               "out_of_range_weights sample-coords reject label");

void testCacheIndexRejectReasons() {

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 0u, 8u, reason),
               "tryIsCacheIndexValid succeeds for origin probe");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None, "valid cache index reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "none") == 0,
               "none cache-index reject label");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 99u, 8u, reason),
               "tryIsCacheIndexValid rejects OOB probe index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::ProbeIndexOutOfRange,
               "OOB probe index reports ProbeIndexOutOfRange");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "probe_index_out_of_range") == 0,
               "probe_index_out_of_range cache-index reject label");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 3u, 2u, reason),
               "tryIsCacheIndexValid rejects undersized cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::CacheUndersized,
               "undersized cache reports CacheUndersized");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "cache_undersized") == 0,
               "cache_undersized cache-index reject label");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(empty, 0u, 8u, reason),
               "tryIsCacheIndexValid rejects empty grid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid cache index reports EmptyGrid");

void testLaunchRejectReasons() {

               "tryCanLaunch succeeds for in-range indices");
               "none launch reject label");

               "tryCanLaunch rejects OOB probe indices");
               "OOB probe index reports OutOfRangeIndex");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "out_of_range_index") == 0,
               "out_of_range_index launch reject label");

               "tryCanLaunch rejects null index buffer");
               "null indices report NullIndices");

               "tryCanLaunch rejects zero probe count");
               "zero count reports ZeroCount");

               "tryCanLaunch rejects empty grid");
               "empty grid launch reports EmptyGrid");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid launch reject label");

void testKernelPreflightGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    params.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "probe trace preflight accepts valid params");
               "valid trace params report no reject reason");
               "none kernel reject label");
               "probe trace launch succeeds with valid params");

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeBlendKernel(params, reason),
               "probe blend preflight accepts valid params");
               "probe blend launch succeeds with valid params");

    params.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "probe trace preflight rejects null indices");
               "null indices report NullIndices for trace");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "probe trace launch rejects null indices");

    params.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(params, reason),
               "probe blend preflight rejects zero count");
               "zero count reports ZeroCount for blend");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(params, nullptr),
               "probe blend launch rejects zero count");

    params.probe_update_count = 1u;
    params.rays_per_probe = 0u;
               "probe trace preflight rejects zero rays per probe");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroRaysPerProbe,
               "zero rays per probe reports ZeroRaysPerProbe");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe kernel reject label");
























    expectTrue(fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 3u, 8u, reason),
               "tryIsCacheIndexValid succeeds for valid cache lookup");
               "valid cache lookup reports none reject reason");

    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "empty_grid") == 0,
               "empty grid cache lookup reports empty_grid");

               "tryIsCacheIndexValid rejects invalid probe index");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "invalid_probe_index") == 0,
               "invalid probe index reports invalid_probe_index");

    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "undersized_cache") == 0,
               "undersized cache reports undersized_cache");

void testProbeSampleCoordsPreflight() {
    desc.probe_spacing = {1.f, 1.f, 1.f};

    expectTrue(fuse::renderer::ProbeGridLayout::canBuildProbeSampleCoords(desc),
               "canBuildProbeSampleCoords true for valid grid");

    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   desc, {0.5f, 0.5f, 0.5f}, built, reason),
               "tryBuildProbeSampleCoords succeeds on valid grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "tryBuild reject reason is None on success");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, built),
               "tryBuild output passes validity guard");

    expectTrue(!fuse::renderer::ProbeGridLayout::canBuildProbeSampleCoords(empty),
               "canBuildProbeSampleCoords false on empty grid");
    fuse::renderer::ProbeSampleCoords emptyBuilt{};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   empty, {0.f, 0.f, 0.f}, emptyBuilt, reason),
               "tryBuildProbeSampleCoords fails on empty grid");
               "tryBuild reports EmptyGrid");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 1.f, 1.f};
    expectTrue(!fuse::renderer::ProbeGridLayout::canBuildProbeSampleCoords(badSpacing),
               "canBuildProbeSampleCoords false on zero spacing");
                   badSpacing, {0.5f, 0.5f, 0.5f}, built, reason),
               "tryBuildProbeSampleCoords fails on invalid spacing");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::InvalidSpacing,
               "tryBuild reports InvalidSpacing");

    fuse::renderer::ProbeSampleCoords messy{};
    messy.x0 = 1u;
    messy.x1 = 0u;
    messy.y0 = 1u;
    messy.y1 = 0u;
    messy.z0 = 1u;
    messy.z1 = 0u;
    messy.tx = 2.f;
    messy.ty = -1.f;
    messy.tz = 0.5f;
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeSampleCoordsNormalized(messy),
               "messy coords fail normalized guard");
    fuse::renderer::ProbeGridLayout::sanitizeProbeSampleCoords(desc, messy);
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsNormalized(messy),
               "sanitize yields normalized coords");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, messy),
               "sanitized coords pass validity guard");

    expectTrue(std::string(fuse::renderer::probeSampleCoordsRejectReasonLabel(
                   fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid)) == "empty_grid",
               "probe sample coord reject label");

void testCacheIndexPreflight() {
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::canLookupCacheAtIndex(desc, 8u),
               "canLookupCacheAtIndex true when cache sized for grid");
    expectTrue(!fuse::renderer::ddgi_util::canLookupCacheAtIndex(desc, 4u),
               "canLookupCacheAtIndex false when cache undersized");

    expectTrue(fuse::renderer::ddgi_util::tryCanLookupCacheAtProbeIndex(desc, 3u, 8u, reason),
               "tryCanLookupCacheAtProbeIndex succeeds for in-range index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "cache lookup reject reason is None on success");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtProbeIndex(desc, 99u, 8u, reason),
               "tryCanLookupCacheAtProbeIndex rejects OOB index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeIndex,
               "cache lookup reports OutOfRangeIndex");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtProbeIndex(desc, 0u, 4u, reason),
               "tryCanLookupCacheAtProbeIndex rejects undersized cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "cache lookup reports UndersizedCache");

    expectTrue(!fuse::renderer::ddgi_util::canLookupCacheAtIndex(empty, 8u),
               "canLookupCacheAtIndex false on empty grid");
    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtProbeIndex(empty, 0u, 8u, reason),
               "tryCanLookupCacheAtProbeIndex rejects empty grid");
               "cache lookup reports EmptyGrid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtProbeIndex(zeroRes, 0u, 8u, reason),
               "tryCanLookupCacheAtProbeIndex rejects not-sampleable grid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NotSampleable,
               "cache lookup reports NotSampleable");

    expectTrue(fuse::renderer::ddgi_util::isCacheIndexValidForClampedIndex(desc, 99u, 8u),
               "clamped OOB index valid in full cache");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValidForClampedIndex(desc, 99u, 4u),
               "clamped OOB index invalid in undersized cache");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValidForClampedIndex(empty, 0u, 8u),
               "clamped index invalid on empty grid");

    expectTrue(std::string(fuse::renderer::cacheIndexRejectReasonLabel(
                   fuse::renderer::CacheIndexRejectReason::UndersizedCache)) == "undersized_cache",
               "cache index reject label");

void testKernelLaunchPreflight() {
    fuse::renderer::gi::DDGIKernelParams valid{};
    valid.probe_indices_to_update = indices;
    valid.probe_update_count = 2u;
    valid.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::canLaunchProbeTraceKernel(valid),
               "canLaunchProbeTraceKernel accepts valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeBlendKernel(valid),
               "canLaunchProbeBlendKernel accepts valid params");

    expectTrue(fuse::renderer::gi::preflightDDGIKernelParams(valid, reason),
               "preflightDDGIKernelParams succeeds for valid params");
               "kernel reject reason is None on success");

    fuse::renderer::gi::DDGIKernelParams zeroCount = valid;
    expectTrue(!fuse::renderer::gi::canLaunchProbeTraceKernel(zeroCount),
               "kernel preflight rejects zero probe count");
    expectTrue(!fuse::renderer::gi::preflightDDGIKernelParams(zeroCount, reason),
               "preflight reports zero probe count");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroProbeCount,
               "kernel reject reason ZeroProbeCount");

    fuse::renderer::gi::DDGIKernelParams nullIndices = valid;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::canLaunchProbeBlendKernel(nullIndices),
               "kernel preflight rejects null probe indices");
    expectTrue(!fuse::renderer::gi::preflightDDGIKernelParams(nullIndices, reason),
               "preflight reports null probe indices");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::NullProbeIndices,
               "kernel reject reason NullProbeIndices");

    fuse::renderer::gi::DDGIKernelParams zeroRays = valid;
    expectTrue(!fuse::renderer::gi::canLaunchProbeTraceKernel(zeroRays),
               "kernel preflight rejects zero rays per probe");
    expectTrue(!fuse::renderer::gi::preflightDDGIKernelParams(zeroRays, reason),
               "preflight reports zero rays per probe");
               "kernel reject reason ZeroRaysPerProbe");

    expectTrue(std::string(fuse::renderer::gi::ddgiKernelRejectReasonLabel(
                   fuse::renderer::gi::DdgiKernelRejectReason::NullProbeIndices)) == "null_probe_indices",
               "kernel reject label");

void testProbeSampleCoordPreflightDiagnostics() {
    desc.grid_origin = {0.f, 0.f, 0.f};

               "canBuildProbeSampleCoords true for sampleable grid");

    fuse::renderer::ProbeSampleCoordsRejectReason buildReason =
                   desc, {0.5f, 0.5f, 0.5f}, built, buildReason),
    expectTrue(buildReason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "tryBuildProbeSampleCoords reason is None on success");

    fuse::renderer::ProbeSampleCoordsRejectReason validateReason =
    expectTrue(fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, built, validateReason),
               "tryValidateProbeSampleCoords accepts built coords");
    expectTrue(validateReason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "tryValidateProbeSampleCoords reason is None on success");

    fuse::renderer::ProbeSampleCoordsRejectReason emptyReason =
                   empty, {0.f, 0.f, 0.f}, emptyBuilt, emptyReason),
    expectTrue(emptyReason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid build reason is EmptyGrid");
    expectTrue(std::string(fuse::renderer::probeSampleCoordsRejectReasonLabel(emptyReason)) == "empty_grid",
               "empty grid build reason label");

    fuse::renderer::ProbeSampleCoordsRejectReason spacingReason =
               "canBuildProbeSampleCoords false for zero spacing axis");
                   badSpacing, {0.5f, 0.5f, 0.5f}, built, spacingReason),
               "tryBuildProbeSampleCoords rejects invalid spacing");
    expectTrue(spacingReason == fuse::renderer::ProbeSampleCoordsRejectReason::InvalidSpacing,
               "invalid spacing build reason");

    oobIndices.y0 = 0u;
    oobIndices.y1 = 1u;
    oobIndices.z0 = 0u;
    oobIndices.z1 = 1u;
    fuse::renderer::ProbeSampleCoordsRejectReason oobReason =
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, oobIndices, oobReason),
               "tryValidateProbeSampleCoords rejects OOB indices");
    expectTrue(oobReason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "OOB indices validation reason");

    fuse::renderer::ProbeSampleCoords unordered = built;
    unordered.x0 = 1u;
    unordered.x1 = 0u;
    fuse::renderer::ProbeSampleCoordsRejectReason unorderedReason =
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, unordered, unorderedReason),
               "tryValidateProbeSampleCoords rejects unordered corners");
    expectTrue(unorderedReason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "unordered corners validation reason");

    fuse::renderer::ProbeSampleCoords badWeights = built;
    badWeights.tx = 2.f;
    fuse::renderer::ProbeSampleCoordsRejectReason weightReason =
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, badWeights, weightReason),
               "tryValidateProbeSampleCoords rejects invalid weights");
    expectTrue(weightReason == fuse::renderer::ProbeSampleCoordsRejectReason::InvalidWeights,
               "invalid weights validation reason");

void testCacheIndexPreflightDiagnostics() {

               "tryIsCacheIndexValid accepts origin index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None, "cache index reason None on success");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 8u, 8u, reason),
               "tryIsCacheIndexValid rejects probe index equal to cache length");
               "probe index equal to probe count rejected as out of range");

               "undersized cache reason");
    expectTrue(std::string(fuse::renderer::cacheIndexRejectReasonLabel(reason)) == "cache_undersized",
               "cache undersized reason label");

    reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheSizedForGrid(empty, 8u, reason),
               "tryValidateCacheSizedForGrid rejects empty grid");
               "empty grid cache sizing reason");

    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheSizedForGrid(desc, 8u, reason),
               "tryValidateCacheSizedForGrid accepts full cache");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheSizedForGrid(desc, 4u, reason),
               "tryValidateCacheSizedForGrid rejects partial cache");
               "partial cache sizing reason");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, 8u),
               "in-range cache index does not skip lookup");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u),
               "null cache skips lookup preflight");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 99u, 8u),
               "OOB probe index skips lookup preflight");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, cache.data(), 99u, 8u),
               "OOB probe index skips lookup even with valid cache pointer");
}

void testLaunchProbeUpdateGuards() {
void testLaunchProbeUpdateIndexGuard() {
}

void testLaunchGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 7u};
    expectTrue(fuse::renderer::canLaunchDdgiProbeUpdate(desc, validIndices, 2u),
               "preflight accepts in-range probe indices");
    expectTrue(fuse::renderer::launch_ddgi_probe_update(desc, validIndices, 2u, nullptr),
               "launch accepts in-range probe indices");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::canLaunchDdgiProbeUpdate(desc, oobIndices, 2u),
               "preflight rejects OOB probe indices");
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(desc, oobIndices, 2u, nullptr),
               "launch rejects OOB probe indices");

    expectTrue(!fuse::renderer::canLaunchDdgiProbeUpdate(desc, nullptr, 1u),
               "preflight rejects null index buffer");
    expectTrue(!fuse::renderer::canLaunchDdgiProbeUpdate(desc, validIndices, 0u),
               "preflight rejects zero probe count");

    expectTrue(!fuse::renderer::canLaunchDdgiProbeUpdate(empty, validIndices, 2u),
               "preflight rejects empty grid");

void testProbeSampleCoordPreflight() {
void testWouldSkipProbeSampleCoords() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "buildProbeSampleCoords for wouldSkip test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");
    expectTrue(fuse::renderer::ProbeGridLayout::canBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}),
               "canBuildProbeSampleCoords true for interior sample");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered corners");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::canBuildProbeSampleCoords(empty, {0.f, 0.f, 0.f}),
               "canBuildProbeSampleCoords false on empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(empty, built),
               "wouldSkip true on empty grid");
}

void testWouldSkipTrilinearProbeSample() {
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "wouldSkip false for valid trilinear sample");

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for wouldSkipTrilinearSampleAtCoords test");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearSampleAtCoords(desc, coords, cache.data(), 8u),
               "wouldSkip false for valid coord-based sample");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u),
               "wouldSkip true for null cache trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearSampleAtCoords(desc, coords, nullptr, 8u),
               "wouldSkip true for null cache coord-based sample");
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u),
               "wouldSkip true for undersized cache trilinear sample");

                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u),
               "wouldSkip true for empty grid trilinear sample");

void testWouldSkipReadIrradianceAtIndex() {

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 8u, 3u),
               "wouldSkip false for valid cache read");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, nullptr, 8u, 3u),
               "wouldSkip true for null cache read");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 4u, 3u),
               "wouldSkip true for undersized cache read");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 8u, 99u),
               "wouldSkip true for OOB probe index read");

void testWouldSkipProbeKernels() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip false for valid trace kernel params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip false for valid blend kernel params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkip true for zero trace update count");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroCount),
               "wouldSkip true for zero blend update count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(nullIndices),
               "wouldSkip true for null trace probe indices");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip true for null blend probe indices");

    fuse::renderer::gi::DDGIKernelParams zeroRays = validParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroRays),
               "wouldSkip true for zero rays per probe");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroRays),
               "wouldSkip true for zero rays per probe on blend");

void testProbeSampleCoordRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for preflight test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason = fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, built, reason),
               "valid coords pass grid-only preflight");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "valid preflight reports no reject reason");
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "canPreflight succeeds on valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "valid coords would not clamp");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, oobWeights, reason),
               "OOB weights pass soft preflight for clamp path");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "OOB weights report out_of_range_weights on soft preflight");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, oobWeights),
               "OOB weights would clamp");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, oobIndices, reason),
               "OOB indices fail hard preflight");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "OOB indices report out_of_range_indices on hard preflight");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, reversed, reason),
               "unordered corners fail hard preflight");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "unordered corners report unordered_corners on hard preflight");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(empty, built, reason),
               "empty grid fails preflight");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid preflight reports empty_grid");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(empty, built),
               "empty grid wouldClamp returns false");
}

void testProbeSchedulePreflightGuards() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, 4u, indices, 8u, &count, reason),
               "trySchedule succeeds with valid outputs");
    expectTrue(count == 4u, "trySchedule writes expected count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");
    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(8u, indices, 8u, &count),
               "canSchedule succeeds with valid outputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(8u, indices, 8u, &count),
               "wouldSkip false for valid schedule");

    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 4u, indices, 8u, &count, reason),
               "zero probe count vacuously succeeds");
    expectTrue(count == 0u, "zero probe count schedules zero probes");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "zero probe count reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, 4u, nullptr, 8u, &count, reason),
               "null output buffer fails schedule preflight");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputBuffer,
               "null output buffer reports null_output_buffer reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "null_output_buffer") == 0,
               "null_output_buffer schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, 4u, indices, 8u, nullptr, reason),
               "null output count fails schedule preflight");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputCount,
               "null output count reports null_output_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, 4u, indices, 0u, &count, reason),
               "zero max_indices fails schedule preflight");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max_indices reports zero_max_indices reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(8u, indices, 0u, &count),
               "wouldSkip true for zero max_indices");
}

void testCacheLookupValidation() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheLookup(desc, cache.data(), 8u, 3u, reason),
               "tryValidateCacheLookup succeeds for in-range index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache lookup reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheLookup(desc, nullptr, 8u, 3u, reason),
               "tryValidateCacheLookup rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache reject reason label");

    expectTrue(fuse::renderer::ddgi_util::shouldSkipCacheLookup(desc, nullptr, 8u),
               "shouldSkipCacheLookup true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipCacheLookup(desc, cache.data(), 8u),
               "shouldSkipCacheLookup false for accessible cache");

    expectTrue(!fuse::renderer::ddgi_util::wouldClampProbeIndex(3u, desc),
               "in-range probe index would not clamp");
    expectTrue(fuse::renderer::ddgi_util::wouldClampProbeIndex(99u, desc),
               "OOB probe index would clamp");
}

void testKernelWouldSkipAndTryLaunch() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip false for valid trace params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip false for valid blend params");

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch trace succeeds for valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful trace tryLaunch reports no reject reason");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch blend succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkip true for zero update count");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch trace rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "tryLaunch trace zero count reports zero_update_count reason");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip true for null probe indices");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(nullIndices, nullptr, reason),
               "tryLaunch blend rejects null probe indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "tryLaunch blend null indices reports null_probe_indices reason");
}

void testProbeSampleCoordRejectReasons() {

               "buildProbeSampleCoords produces coords for reject-reason test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason = fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, built, reason),
               "valid sample coords pass tryValidate");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None, "valid coords report no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "none") == 0,
               "none sample-coord reject reason label");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, built),
               "valid coords are not out of range");

    fuse::renderer::ProbeSampleCoords reversed = built;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, reversed, reason),
               "unordered corners fail tryValidate");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "unordered corners report correct reject reason");
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, reversed),
               "unordered corners flagged out of range");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, oobIndices, reason),
               "OOB indices fail tryValidate");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "OOB indices report out_of_range_indices reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "out_of_range_indices") == 0,
               "out_of_range_indices reject reason label");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(desc, oobWeights, reason),
               "OOB weights fail tryValidate");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "OOB weights report out_of_range_weights reason");

    fuse::renderer::ProbeSampleCoords unclamped = oobIndices;
    const fuse::u32 originalX0 = unclamped.x0;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(empty, unclamped),
               "tryClampProbeSampleCoords rejects empty grid");
    expectTrue(unclamped.x0 == originalX0, "tryClampProbeSampleCoords leaves coords unchanged on empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, unclamped),
               "tryClampProbeSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, unclamped),
               "tryClampProbeSampleCoords yields valid coords");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeSampleCoords(empty, built, reason),
               "empty grid fails sample-coord validation");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid reports empty_grid reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid sample-coord reject reason label");

void testCacheIndexNullCacheGuard() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, cache.data(), 3u, 8u, reason),
               "cache pointer overload accepts valid lookup");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache pointer lookup reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, nullptr, 3u, 8u, reason),
               "cache pointer overload rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache cache-index reject reason label");
}

void testCacheIndexRejectReasons() {

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, 3u, 8u, reason),
               "in-range cache index passes tryValidate");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None, "valid cache index reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "none") == 0,
               "none cache-index reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, 99u, 8u, reason),
               "OOB probe index fails tryValidate");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB probe index reports out_of_range_probe_index reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "out_of_range_probe_index") == 0,
               "out_of_range_probe_index reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, 3u, 2u, reason),
               "undersized cache fails tryValidate");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "undersized_cache") == 0,
               "undersized_cache reject reason label");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, cache.data(), 3u, 8u, reason),
               "cache-pointer overload passes for valid index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "cache-pointer overload reports no reject reason on success");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, nullptr, 3u, 8u, reason),
               "null cache fails cache-pointer tryValidate");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, 8u),
               "wouldSkip false for valid cache-index lookup");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u),
               "wouldSkip true for null cache lookup");

    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, 8u, &cacheReason),
               "wouldSkip with reason false for valid cache-index lookup");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache-index wouldSkip reason is none");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u, &cacheReason),
               "wouldSkip with reason true for null cache lookup");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache wouldSkip reports null_cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 99u, 8u, &cacheReason),
               "wouldSkip with reason true for OOB probe index");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB probe index wouldSkip reports out_of_range_probe_index");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(empty, 0u, 8u, reason),
               "empty grid fails cache-index validation");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid reports empty_grid cache reject reason");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, cache.data(), 3u, 8u, reason),
               "cache-pointer overload passes for in-range index");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, nullptr, 3u, 8u, reason),
               "null cache fails cache-pointer tryValidate");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache reject reason label");
               "null cache fails cache-index validation with pointer overload");
               "null cache reports null_cache reject reason");
               "null_cache cache-index reject reason label");
               "non-null cache passes cache-index validation with pointer overload");
               "null cache fails cache-index validation with cache pointer");
}

void testProbeSchedulePreflights() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeSchedule(2048u, 64u, indices, 8u, &count, reason),
               "tryValidateProbeSchedule succeeds for valid inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");
    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 64u, indices, 8u, &count),
               "canScheduleProbeUpdates true for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, 8u, &count),
               "wouldSkipProbeSchedule false for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeSchedule(2048u, 64u, nullptr, 8u, &count, reason),
               "tryValidateProbeSchedule rejects null indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullIndices,
               "null indices report null_indices schedule reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "null_indices") == 0,
               "null_indices schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeSchedule(2048u, 64u, indices, 8u, nullptr, reason),
               "tryValidateProbeSchedule rejects null count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullCount,
               "null count reports null_count schedule reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeSchedule(0u, 64u, indices, 8u, &count, reason),
               "tryValidateProbeSchedule rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count schedule reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeSchedule(2048u, 64u, indices, 0u, &count, reason),
               "tryValidateProbeSchedule rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices schedule reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, 8u, &count),
               "wouldSkipProbeSchedule true for zero probe count");

void testWouldSkipProbeSampleCoords() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldSkip test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built) ==
                   fuse::renderer::ProbeGridLayout::isProbeSampleCoordsOutOfRange(desc, built),
               "wouldSkip matches isProbeSampleCoordsOutOfRange");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered corners");

void testProbeKernelWouldSkipAndTryLaunch() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkipProbeTraceKernel false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkipProbeBlendKernel false for valid params");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_trace_kernel succeeds for valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful tryLaunch trace reports no reject reason");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_blend_kernel succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkipProbeTraceKernel true for zero update count");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch_probe_trace_kernel rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "tryLaunch trace zero count reports zero_update_count reason");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkipProbeBlendKernel true for null probe indices");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(nullIndices, nullptr, reason),
               "tryLaunch_probe_blend_kernel rejects null probe indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "tryLaunch blend null indices reports null_probe_indices reason");
}

void testCacheAccessRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheAccess(desc, cache.data(), 8u, 3u, reason),
               "tryValidateCacheAccess succeeds for accessible cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "accessible cache reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheAccess(desc, nullptr, 8u, 3u, reason),
               "tryValidateCacheAccess rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache reject reason label");
}

void testLaunchProbeUpdateRejectReasons() {

    fuse::renderer::ProbeUpdateLaunchRejectReason reason = fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "tryCanLaunch succeeds for valid indices");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "valid launch reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "none") == 0,
               "none launch reject reason label");

    fuse::u32 duplicateIndices[2] = {3u, 3u};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, duplicateIndices, 2u, reason),
               "tryCanLaunch rejects duplicate probe indices");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::DuplicateProbeIndex,
               "duplicate indices report duplicate_probe_index reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "duplicate_probe_index") == 0,
               "duplicate_probe_index launch reject reason label");
    expectTrue(fuse::renderer::wouldSkipDdgiProbeUpdate(desc, duplicateIndices, 2u),
               "wouldSkip true for duplicate probe indices");
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(desc, duplicateIndices, 2u, nullptr),
               "launch rejects duplicate probe indices");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, oobIndices, 2u, reason),
               "tryCanLaunch rejects OOB probe indices");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "OOB probe index reports out_of_range_probe_index launch reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "out_of_range_probe_index") ==
                   0,
               "out_of_range_probe_index launch reject reason label");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, nullptr, 1u, reason),
               "tryCanLaunch rejects null index buffer");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::NullIndices,
               "null indices report null_indices launch reason");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 0u, reason),
               "tryCanLaunch rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::ZeroCount,
               "zero count reports zero_count launch reason");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(empty, validIndices, 2u, reason),
               "tryCanLaunch rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::EmptyGrid,
               "empty grid reports empty_grid launch reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid launch reject reason label");

void testProbeSampleCoordPreflightHelpers() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for preflight helper test");
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "built coords pass grid-only preflight");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "built coords do not need clamp");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, reversed),
               "reversed corners still pass grid-only preflight");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, reversed),
               "reversed corners need normalize/clamp");

    fuse::renderer::ProbeSampleCoords normalized = reversed;
    expectTrue(fuse::renderer::ProbeGridLayout::tryNormalizeAndValidateProbeSampleCoords(desc, normalized),
               "tryNormalizeAndValidate fixes reversed corners");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, normalized),
               "normalized coords pass full validity guard");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::renderer::ProbeSampleCoords emptyCoords = built;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryNormalizeAndValidateProbeSampleCoords(empty, emptyCoords),
               "tryNormalizeAndValidate rejects empty grid");
}

void testProbeSampleCoordsInBounds() {

               "buildProbeSampleCoords for in-bounds test");
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, built),
               "built sample coords are in bounds");

    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, reversed),
               "reversed corners still in bounds before normalize");
               "reversed corners fail validity despite in-bounds indices");

    expectTrue(!fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, oobIndices),
               "OOB indices fail areProbeSampleCoordsInBounds");

    expectTrue(!fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, oobWeights),
               "OOB weights fail areProbeSampleCoordsInBounds");

    expectTrue(!fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(empty, built),
               "empty grid sample coords not in bounds");

void testTryBuildProbeSampleCoords() {

    fuse::renderer::ProbeSampleCoords coords{};
    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords, reason),
               "tryBuildProbeSampleCoords succeeds on interior sample");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "interior build reports no reject reason");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(empty, {0.f, 0.f, 0.f}, coords, reason),
               "tryBuildProbeSampleCoords rejects empty grid");
               "empty grid build reports empty_grid reason");
               "empty_grid sample-coord build reject reason label");

    fuse::renderer::DDGIDesc zeroSpacing = desc;
    zeroSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   zeroSpacing, {0.5f, 0.5f, 0.5f}, coords, reason),
               "tryBuildProbeSampleCoords rejects non-sampleable grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::NotSampleableGrid,
               "zero spacing build reports not_sampleable_grid reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "not_sampleable_grid") == 0,
               "not_sampleable_grid sample-coord reject reason label");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
                   zeroRes, {0.5f, 0.5f, 0.5f}, coords, reason),
               "tryBuildProbeSampleCoords rejects zero irradiance_res grid");
               "zero irradiance_res build reports not_sampleable_grid reason");

    fuse::renderer::DDGIDesc nonSampleable = desc;
    nonSampleable.probe_spacing = {0.f, 1.f, 1.f};
                   nonSampleable, {0.5f, 0.5f, 0.5f}, coords, reason),
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::NonSampleableGrid,
               "zero spacing reports non_sampleable_grid reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "non_sampleable_grid") == 0,
               "non_sampleable_grid sample-coord reject reason label");
}

void testProbeCacheAccessibilityGuards() {
    desc.irradiance_res = 8;

    expectTrue(!fuse::renderer::ddgi_util::shouldSkipProbeGrid(desc), "sampleable grid not skipped");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipProbeLookup(desc, cache.data(), 8u),
               "full cache lookup not skipped");
    expectTrue(fuse::renderer::ddgi_util::isProbeCacheAccessible(desc, cache.data(), 8u),
               "full cache is accessible");

    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeLookup(desc, nullptr, 8u),
               "null cache lookup skipped");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeLookup(desc, cache.data(), 4u),
               "undersized cache lookup skipped");

    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeGrid(empty), "empty grid skipped");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeLookup(empty, cache.data(), 8u),
               "empty grid lookup skipped");

void testTryReadIrradianceAtIndex() {

    cache[3u].irradiance = {0.25f, 0.5f, 0.75f};

    fuse::math::Vec3 irradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, cache.data(), 8u, 3u, irradiance, reason),
               "tryReadIrradianceAtIndex succeeds for in-range index");
    expectNear(irradiance.x, 0.25f, 1e-5f, "tryRead returns stored irradiance x");
               "successful tryRead reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, nullptr, 8u, 3u, irradiance, reason),
               "tryRead rejects null cache");
               "null cache tryRead reports null_cache reason");

                   desc, cache.data(), 4u, 3u, irradiance, reason),
               "tryRead rejects undersized cache");
               "undersized cache tryRead reports undersized_cache reason");

                   desc, cache.data(), 8u, 99u, irradiance, reason),
               "tryRead rejects OOB probe index");
               "OOB probe index tryRead reports out_of_range_probe_index reason");

void testTryCanSampleAtProbeCoords() {

    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for sample preflight test");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, coords, cache.data(), 8u, reason),
               "tryCanSampleAtProbeCoords succeeds on accessible grid");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "valid sample preflight reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeTrilinearSampleRejectReasonLabel(reason), "none") == 0,
               "none trilinear reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, coords, nullptr, 8u, reason),
               "tryCanSampleAtProbeCoords rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, coords, cache.data(), 4u, reason),
               "tryCanSampleAtProbeCoords rejects undersized cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::UndersizedCache,

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, invalid, cache.data(), 8u, reason),
               "tryCanSampleAtProbeCoords succeeds for fixable unordered corners via preflight path");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "fixable unordered corners report no trilinear reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(zeroRes, coords, cache.data(), 8u, reason),
               "tryCanSampleAtProbeCoords rejects non-sampleable grid");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NotSampleable,
               "zero irradiance_res reports not_sampleable reason");

void testTryTrilinearProbeIrradiance() {
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};

    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {static_cast<fuse::f32>(i), 0.f, 0.f};

    fuse::math::Vec3 sampled{};
    expectTrue(fuse::renderer::ddgi_util::tryTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, sampled, reason),
               "tryTrilinearProbeIrradiance succeeds on full cache");
    expectNear(sampled.x, 3.5f, 1e-4f, "tryTrilinear matches centre average");
               "successful trilinear sample reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u, sampled, reason),
               "tryTrilinearProbeIrradiance rejects undersized cache");
               "undersized trilinear sample reports undersized_cache reason");

                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u, sampled, reason),
               "tryTrilinearProbeIrradiance rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "empty grid trilinear sample reports empty_grid reason");

void testTryTrilinearDirectionalProbeIrradiance() {

        cache[i].irradiance = {1.f, 1.f, 1.f};

    expectTrue(fuse::renderer::ddgi_util::tryTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 8u, sampled),
               "tryTrilinearDirectionalProbeIrradiance succeeds on full cache");
    expectTrue(sampled.x > 0.f, "directional trilinear sample is non-zero");

    expectTrue(!fuse::renderer::ddgi_util::tryTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, nullptr, 8u, sampled, reason),
               "tryTrilinearDirectional rejects null cache");
               "directional trilinear null cache reports null_cache reason");

void testWouldSkipDdgiProbeUpdate() {

    expectTrue(!fuse::renderer::wouldSkipDdgiProbeUpdate(desc, validIndices, 2u),
               "wouldSkip false for valid launch");

    expectTrue(fuse::renderer::wouldSkipDdgiProbeUpdate(desc, oobIndices, 2u),
               "wouldSkip true for OOB indices");
    expectTrue(fuse::renderer::wouldSkipDdgiProbeUpdate(desc, nullptr, 1u),
               "wouldSkip true for null indices");

    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(!fuse::renderer::wouldSkipDdgiProbeUpdate(desc, validIndices, 2u, &launchReason),
               "wouldSkip with reason false for valid launch");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "valid launch wouldSkip reason is none");
    expectTrue(fuse::renderer::wouldSkipDdgiProbeUpdate(desc, oobIndices, 2u, &launchReason),
               "wouldSkip with reason true for OOB indices");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "OOB launch wouldSkip reports out_of_range_probe_index");
}

void testWouldSkipGuardOverloads() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for wouldSkip overload tests");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, coords),
               "wouldSkipProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeSampleCoords reversed = coords;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    fuse::renderer::ProbeSampleCoordsRejectReason sampleReason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed, sampleReason),
               "wouldSkipProbeSampleCoords true for unordered corners");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "wouldSkipProbeSampleCoords reports unordered_corners reason");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for accessible cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(
                   desc, coords, nullptr, 8u, trilinearReason),
               "wouldSkipProbeTrilinearSample true for null cache");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "wouldSkipProbeTrilinearSample reports null_cache reason");

    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, 8u, cacheReason),
               "wouldSkipCacheIndexLookup false for valid index");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache-index wouldSkip reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u, cacheReason),
               "wouldSkipCacheIndexLookup true for null cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache wouldSkip reports null_cache reason");
    expectTrue(fuse::renderer::ddgi_util::isCacheIndexValid(desc, cache.data(), 3u, 8u),
               "isCacheIndexValid pointer overload accepts valid index");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValid(desc, cache.data(), 3u, 4u),
               "isCacheIndexValid pointer overload rejects undersized cache for grid");

    fuse::math::Vec3 irradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, cache.data(), 8u, 3u, irradiance, cacheReason),
               "tryReadIrradianceAtIndex reason overload succeeds");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "successful tryRead reports no cache reject reason");
    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, cache.data(), 4u, 3u, irradiance, cacheReason),
               "tryRead reason overload rejects grid-undersized cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "grid-undersized tryRead reports undersized_cache reason");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count, scheduleReason),
               "wouldSkipProbeSchedule false for valid inputs");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule wouldSkip reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, &count, scheduleReason),
               "wouldSkipProbeSchedule true for zero probe count");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count wouldSkip reports zero_probe_count reason");

    fuse::u32 validLaunch[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(!fuse::renderer::wouldSkipDdgiProbeUpdate(desc, validLaunch, 2u, launchReason),
               "wouldSkipDdgiProbeUpdate reason overload false for valid launch");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "valid launch wouldSkip reports no reject reason");
    fuse::u32 oobLaunch[2] = {0u, 99u};
    expectTrue(fuse::renderer::wouldSkipDdgiProbeUpdate(desc, oobLaunch, 2u, launchReason),
               "wouldSkipDdgiProbeUpdate reason overload true for OOB indices");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "OOB launch wouldSkip reports out_of_range_probe_index reason");

    fuse::u32 kernelIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = kernelIndices;
    kernelParams.probe_update_count = 2u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(kernelParams, kernelReason),
               "wouldSkipProbeTraceKernel false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(kernelParams, kernelReason),
               "wouldSkipProbeBlendKernel false for valid params");
    kernelParams.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(kernelParams, kernelReason),
               "wouldSkipProbeTraceKernel true for zero update count");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "zero update count wouldSkip reports zero_update_count reason");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(kernelParams),
               "wouldSkipProbeBlendKernel true without reason param");
}

void testTryLaunchDdgiProbeUpdate() {

    fuse::renderer::ProbeUpdateLaunchRejectReason reason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryLaunch_ddgi_probe_update(desc, validIndices, 2u, nullptr, reason),
               "tryLaunch succeeds for valid indices");
               "successful tryLaunch reports no reject reason");

    expectTrue(!fuse::renderer::tryLaunch_ddgi_probe_update(desc, oobIndices, 2u, nullptr, reason),
               "tryLaunch rejects OOB indices");
               "tryLaunch OOB reports out_of_range_probe_index reason");

void testProbeScheduleAtRateGuards() {
void testProbeScheduleCapacityGuards() {
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(2048u, 64u, 64u) == 64u,
               "effectiveScheduledProbeCount matches probes_per_frame when capacity allows");
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(32u, 64u, 64u) == 32u,
               "effectiveScheduledProbeCount limited by probe_count");
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(2048u, 64u, 32u) == 32u,
               "effectiveScheduledProbeCount limited by max_indices");

    expectTrue(fuse::renderer::ddgi_util::wouldClampScheduledProbeCount(2048u, 64u, 32u),
               "wouldClamp true when max_indices below probes_per_frame");
    expectTrue(fuse::renderer::ddgi_util::wouldClampScheduledProbeCount(32u, 64u, 64u),
               "wouldClamp true when probe_count below probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampScheduledProbeCount(2048u, 64u, 64u),
               "wouldClamp false when capacity matches request");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampScheduledProbeCount(0u, 64u, 64u),
               "wouldClamp false for zero probe count");
}

void testProbeSampleCoordPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords inBounds{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, inBounds),
               "build coords for preflight test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, inBounds, reason),
               "tryPreflightProbeSampleCoords succeeds for in-bounds coords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "in-bounds preflight reports no reject reason");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, inBounds),
               "wouldClamp false for valid sample coords");
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, inBounds),
               "canPreflightProbeSampleCoords true for valid coords");

    fuse::renderer::ProbeSampleCoords warnWeights = inBounds;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, warnWeights, reason),
               "tryPreflightProbeSampleCoords warns but succeeds for clampable weights");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "clampable weights report out_of_range_weights reason");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, warnWeights),
               "wouldClamp true for OOB weights");

    fuse::renderer::ProbeSampleCoords hardOob = inBounds;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, hardOob, reason),
               "tryPreflightProbeSampleCoords rejects hard OOB corner indices");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "hard OOB indices report out_of_range_indices reason");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, hardOob),
               "wouldClamp true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord oobCoord{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeGridCoordOutOfRange(desc, oobCoord),
               "isProbeGridCoordOutOfRange true for OOB coord");
    const fuse::renderer::ProbeGridCoord validCoord{1, 1, 1};
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeGridCoordOutOfRange(desc, validCoord),
               "isProbeGridCoordOutOfRange false for valid coord");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(empty, inBounds, reason),
               "tryPreflightProbeSampleCoords rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid preflight reports empty_grid reason");

void testCacheIndexClampAndReadRejectReasons() {

    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndex(99u, desc),
               "wouldClampCacheIndex true for OOB probe index");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndex(3u, desc),
               "wouldClampCacheIndex false for in-range probe index");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    cache[3u].irradiance = {0.25f, 0.5f, 0.75f};

    fuse::math::Vec3 irradiance{};
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, cache.data(), 8u, 3u, irradiance, reason),
               "tryRead with reason succeeds for in-range index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "successful read reports no cache reject reason");
    expectNear(irradiance.x, 0.25f, 1e-5f, "tryRead with reason returns stored irradiance x");

    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, nullptr, 8u, 3u, irradiance, reason),
               "tryRead with reason rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache read reports null_cache reason");

                   desc, cache.data(), 4u, 3u, irradiance, reason),
               "tryRead with reason rejects undersized cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "undersized cache read reports undersized_cache reason");

void testProbeKernelWouldSkipGuards() {
void testWouldSkipProbeSampleCoords() {

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldSkip sample-coord test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built, reason),
               "wouldSkip with reason false for valid sample coords");
               "valid sample coords wouldSkip reason is none");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed, reason),
               "wouldSkip true for unordered corners");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "unordered corners wouldSkip reports unordered_corners");

    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(empty, built, reason),
               "wouldSkip true for empty grid sample coords");
               "empty grid sample-coord wouldSkip reports empty_grid");
}

void testWouldSkipProbeTrilinearSample() {
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for wouldSkip trilinear test");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u, reason),
               "wouldSkip false for accessible trilinear sample");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "accessible trilinear sample wouldSkip reason is none");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, nullptr, 8u, reason),
               "wouldSkip true for null cache trilinear sample");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache trilinear wouldSkip reports null_cache");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 4u),
               "wouldSkip true for undersized cache without reason out-param");

void testWouldSkipKernelLaunch() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip false for valid trace params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip false for valid blend params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkip true for zero update count trace");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroCount),
               "wouldSkip true for zero update count blend");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(nullIndices),
               "wouldSkip true for null probe indices trace");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip true for null probe indices blend");
    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams, &reason),
               "wouldSkip trace false for valid kernel params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "valid trace kernel wouldSkip reason is none");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams, &reason),
               "wouldSkip blend false for valid kernel params");

    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount, &reason),
               "wouldSkip trace true for zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "zero update count kernel wouldSkip reports zero_update_count");
               "wouldSkip blend true for zero update count without reason out-param");
}

void testProbeScheduleRejectReasons() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanScheduleProbeUpdatesAtRate(
                   2048u, 64u, 64u, indices, &count, reason),
               "rate-aware schedule preflight succeeds for valid rate");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid rate reports no reject reason");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "wouldSkip false for valid rate-aware schedule");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdatesAtRate(
                   2048u, 0u, 64u, indices, &count, reason),
               "zero probes_per_frame fails rate-aware schedule preflight");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame reports zero_probes_per_frame reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_probes_per_frame") == 0,
               "zero_probes_per_frame schedule reject reason label");
    expectTrue(count == 0u, "zero probes_per_frame clears scheduled count");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "wouldSkip true for zero probes_per_frame");

    expectTrue(fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, &count, reason),
               "base schedule preflight still succeeds when rate is not checked");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testProbeScheduleRejectReasons() {

               "tryCanSchedule succeeds for valid schedule inputs");
               "valid schedule reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");

    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdates succeeds for valid inputs");
    expectTrue(count == 64u, "trySchedule schedules 64 probes");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(0u, 64u, indices, &count, reason),
               "zero probe count fails schedule preflight");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_probe_count") == 0,
               "zero_probe_count schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 0u, indices, &count, reason),
               "zero max indices fails schedule preflight");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, nullptr, &count, reason),
               "null out indices fails schedule preflight");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutIndices,
               "null out indices reports null_out_indices reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, nullptr, reason),
               "null out count fails schedule preflight");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutCount,
               "null out count reports null_out_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 64u, indices, 64u, &count, reason),
               "trySchedule rejects zero probe count");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, &count),
               "wouldSkip true for zero probe count");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "wouldSkip false for valid schedule inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count, &reason),
               "wouldSkip with reason false for valid schedule inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule wouldSkip reason is none");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, &count, &reason),
               "wouldSkip with reason true for zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count wouldSkip reports zero_probe_count");
}

void testTryClampProbeSampleCoordsRejectReason() {

    coords.x0 = 9u;
    coords.x1 = 9u;
    coords.tx = 2.f;

    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, coords, reason),
               "tryClamp with reason succeeds on non-empty grid");
               "successful clamp reports no reject reason");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, coords),
               "tryClamp with reason yields valid coords");

    const fuse::u32 originalX0 = coords.x0;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(empty, coords, reason),
               "tryClamp with reason rejects empty grid");
               "empty grid clamp reports empty_grid reason");
    expectTrue(coords.x0 == originalX0, "empty grid tryClamp leaves coords unchanged");

void testTryLaunchProbeKernels() {
void testProbeIndexBoundsHelpers() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {4, 2, 3};

    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(desc) == 23u, "maxProbeIndex returns last probe");
    expectTrue(fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(23u, desc),
               "last probe index flagged at max");
    expectTrue(!fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(22u, desc),
               "non-last probe index not at max");

    fuse::u32 clamped = 99u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeIndex(17u, desc, clamped),
               "tryClampProbeIndex succeeds on non-empty grid");
    expectTrue(clamped == 17u, "tryClampProbeIndex preserves in-range index");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    clamped = 42u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeIndex(5u, empty, clamped),
               "tryClampProbeIndex rejects empty grid");
    expectTrue(clamped == 0u, "tryClampProbeIndex zeroes output on empty grid");
}

    desc.grid_dims = {2, 2, 2};

    expectTrue(fuse::renderer::ProbeGridLayout::canBuildProbeSampleCoords(desc),
               "default spacing passes build preflight");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built, reason),
               "successful build reports no reject reason");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, built),
               "tryBuild output passes validity guard");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 1.f, 1.f};
    expectTrue(!fuse::renderer::ProbeGridLayout::canBuildProbeSampleCoords(badSpacing),
               "zero spacing fails build preflight");
                   badSpacing, {0.5f, 0.5f, 0.5f}, built, reason),
               "tryBuildProbeSampleCoords rejects invalid spacing");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::InvalidSpacing,
               "invalid spacing reports correct reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "invalid_spacing") == 0,
               "invalid_spacing sample-coord reject reason label");

                   empty, {0.f, 0.f, 0.f}, built, reason),
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid reports empty_grid build reason");

void testCanSampleAtProbeCoords() {

               "build sample coords for probe-coord preflight test");

    expectTrue(fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, coords, 8u, reason),
               "full cache passes coord-based sample preflight");
               "accessible coord sample reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::canSampleAtProbeCoords(desc, coords, 8u),
               "canSampleAtProbeCoords mirrors tryCanSample success");

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, coords, 4u, reason),
               "undersized cache fails coord-based sample preflight");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache sample reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "undersized_cache") == 0,
               "undersized_cache sample-coord reject reason label");

    invalid.tx = 2.f;
    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, invalid, 8u, reason),
               "invalid weights fail coord-based sample preflight");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "invalid weights report out_of_range_weights sample reason");

    fuse::renderer::DDGIDesc notSampleable = desc;
    notSampleable.irradiance_res = 0u;
    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(notSampleable, coords, 8u, reason),
               "non-sampleable grid fails coord-based sample preflight");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::NotSampleable,
               "non-sampleable grid reports not_sampleable sample reason");

void testExtendedCacheLookupPreflight() {

    expectTrue(fuse::renderer::ddgi_util::canLookupCacheAtIndex(desc, 8u),
               "full cache passes lookup preflight");
    expectTrue(!fuse::renderer::ddgi_util::canLookupCacheAtIndex(desc, 4u),
               "undersized cache fails lookup preflight");

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanLookupCacheAtProbeIndex(desc, 3u, 8u, reason),
               "in-range probe passes extended cache lookup preflight");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "accessible lookup reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtProbeIndex(desc, 3u, 0u, reason),
               "zero cache fails extended lookup preflight");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::ZeroCache,
               "zero cache reports zero_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "zero_cache") == 0,
               "zero_cache cache-index reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtProbeIndex(notSampleable, 0u, 8u, reason),
               "non-sampleable grid fails extended lookup preflight");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NotSampleable,
               "non-sampleable grid reports not_sampleable cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "not_sampleable") == 0,
               "not_sampleable cache-index reject reason label");

    expectTrue(fuse::renderer::ddgi_util::isCacheIndexOutOfRange(desc, 99u, 8u),
               "isCacheIndexOutOfRange mirrors invalid cache index");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexOutOfRange(desc, 3u, 8u),
               "valid cache index not out of range");

void testSampleRequestRejectReasons() {

    fuse::renderer::DDGISampleRequest request{};
    request.world_position = {0.5f, 0.5f, 0.5f};
    request.world_normal = {0.f, 1.f, 0.f};

    fuse::renderer::SampleRequestRejectReason reason = fuse::renderer::SampleRequestRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateSampleRequest(desc, request, 8u, reason),
               "valid sample request passes tryValidate");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::None,
               "valid sample request reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleRequestRejectReasonLabel(reason), "none") == 0,
               "none sample-request reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(desc, request, 4u, reason),
               "undersized cache fails sample-request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache sample-request reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(notSampleable, request, 8u, reason),
               "non-sampleable grid fails sample-request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::NotSampleable,
               "non-sampleable grid reports not_sampleable sample-request reason");
    expectTrue(std::strcmp(fuse::renderer::sampleRequestRejectReasonLabel(reason), "not_sampleable") == 0,
               "not_sampleable sample-request reject reason label");

void testProbeKernelExtendedPreflight() {
void testProbeSchedulePreflight() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ddgi_util::ProbeScheduleRejectReason reason =
        fuse::renderer::ddgi_util::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, 4u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates succeeds on valid inputs");
    expectTrue(reason == fuse::renderer::ddgi_util::ProbeScheduleRejectReason::None,
               "valid schedule reports no reject reason");
    expectTrue(count == 4u, "trySchedule schedules four probes");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 4u, indices, 8u, &count, reason),
               "trySchedule rejects zero probe count");
    expectTrue(reason == fuse::renderer::ddgi_util::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");
    expectTrue(std::strcmp(fuse::renderer::ddgi_util::probeScheduleRejectReasonLabel(reason), "zero_probe_count") == 0,
               "zero_probe_count schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, 4u, nullptr, 8u, &count, reason),
               "trySchedule rejects null indices buffer");
    expectTrue(reason == fuse::renderer::ddgi_util::ProbeScheduleRejectReason::NullIndicesBuffer,
               "null indices reports null_indices_buffer reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, 4u, indices, 8u, nullptr, reason),
               "trySchedule rejects null count out");
    expectTrue(reason == fuse::renderer::ddgi_util::ProbeScheduleRejectReason::NullCountOut,
               "null count out reports null_count_out reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, 4u, indices, 0u, &count, reason),
               "trySchedule rejects zero max indices");
    expectTrue(reason == fuse::renderer::ddgi_util::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");

    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(8u, 4u, indices, 8u, &count),
               "canScheduleProbeUpdates true for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 4u, indices, 8u, &count),
               "wouldSkipProbeSchedule true for zero probe count");

    fuse::u32 zeroPerFrameCount = 99u;
    fuse::renderer::ddgi_util::scheduleProbeUpdates(0u, 8u, 0u, indices, 8u, &zeroPerFrameCount);
    expectTrue(zeroPerFrameCount == 0u, "scheduleProbeUpdates allows zero probes_per_frame");
}

void testTryValidateCacheLookup() {
    fuse::renderer::DDGIDesc desc{};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);

    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheLookup(desc, cache.data(), 8u, 3u, reason),
               "tryValidateCacheLookup succeeds for in-range index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None, "valid cache lookup reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheLookup(desc, nullptr, 8u, 3u, reason),
               "tryValidateCacheLookup rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache cache-index reject reason label");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexValidation(desc, nullptr, 8u, 3u),
               "wouldSkipCacheIndexValidation true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexValidation(desc, cache.data(), 8u, 3u),
               "wouldSkipCacheIndexValidation false for accessible cache");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheLookup(desc, cache.data(), 4u, 3u, reason),
               "tryValidateCacheLookup rejects undersized cache for grid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache reason");

void testProbeSampleCoordPreflight() {

    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for preflight test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason = fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, built, reason),
               "tryPreflightProbeSampleCoords succeeds on valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "canPreflightProbeSampleCoords true on valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, oobIndices),
               "wouldClampProbeSampleCoords true for OOB indices");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "wouldClampProbeSampleCoords false for built coords");

    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    fuse::renderer::ProbeSampleCoords notSampleable{};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   badSpacing, {0.5f, 0.5f, 0.5f}, notSampleable, reason),
               "tryBuildProbeSampleCoords rejects non-sampleable grid");
               "zero spacing reports not_sampleable reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "not_sampleable") == 0,
               "not_sampleable sample-coord reject reason label");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(badSpacing, built, reason),
               "tryPreflightProbeSampleCoords rejects non-sampleable grid");
               "preflight non-sampleable reports not_sampleable reason");

void testProbeKernelBlendPreflight() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::preflightProbeKernelParams(validParams, reason),
               "valid kernel params pass extended preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "valid extended preflight reports no reject reason");

    fuse::renderer::gi::DDGIKernelParams zeroRays = validParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::preflightProbeKernelParams(zeroRays, reason),
               "zero rays_per_probe fails extended preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "zero rays_per_probe reports zero_rays_per_probe reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe kernel reject reason label");

    expectTrue(fuse::renderer::gi::canLaunchProbeTraceKernel(zeroRays),
               "existing trace launch guard unchanged for zero rays_per_probe");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(zeroRays, nullptr),
               "existing trace launch unchanged for zero rays_per_probe");
    validParams.max_ray_distance = 20.f;
    validParams.hysteresis = 0.97f;
    validParams.prev_irradiance_surface = reinterpret_cast<void*>(0x1u);
    validParams.out_radiance_surface = reinterpret_cast<void*>(0x2u);
    validParams.irradiance_atlas_surface = reinterpret_cast<void*>(0x3u);
    validParams.depth_atlas_surface = reinterpret_cast<void*>(0x4u);

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeBlendKernel(validParams, reason),
               "full blend params pass blend preflight");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkipProbeBlendKernel false for valid blend params");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch blend succeeds with valid params");

    fuse::renderer::gi::DDGIKernelParams nullAtlas = validParams;
    nullAtlas.irradiance_atlas_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(nullAtlas, reason),
               "null irradiance atlas fails blend preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullAtlasSurfaces,
               "null atlas reports null_atlas_surfaces reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "null_atlas_surfaces") == 0,
               "null_atlas_surfaces kernel reject reason label");

    fuse::renderer::gi::DDGIKernelParams nullRadiance = validParams;
    nullRadiance.out_radiance_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(nullRadiance, reason),
               "null radiance surface fails blend preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullRadianceSurfaces,
               "null radiance reports null_radiance_surfaces reason");

    fuse::renderer::gi::DDGIKernelParams badHysteresis = validParams;
    badHysteresis.hysteresis = 1.5f;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(badHysteresis, reason),
               "invalid hysteresis fails blend preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::InvalidHysteresis,
               "invalid hysteresis reports invalid_hysteresis reason");

    fuse::renderer::gi::DDGIKernelParams zeroDistance = validParams;
    zeroDistance.max_ray_distance = 0.f;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(zeroDistance, reason),
               "zero max ray distance fails trace preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroMaxRayDistance,
               "zero max ray distance reports zero_max_ray_distance reason");
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroDistance),
               "wouldSkipProbeTraceKernel true for zero max ray distance");

    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroDistance, nullptr, reason),
               "tryLaunch trace rejects zero max ray distance");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(validParams, nullptr),
               "stub blend launch still succeeds without surface wiring");
}

void testProbeSchedulePreflightGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, &count, reason),
               "tryCanScheduleProbeUpdates succeeds for valid buffers");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule preflight reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(0u, 64u, indices, &count, reason),
               "tryCanScheduleProbeUpdates rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_probe_count") == 0,
               "zero_probe_count schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 0u, indices, &count, reason),
               "tryCanScheduleProbeUpdates rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, nullptr, &count, reason),
               "tryCanScheduleProbeUpdates rejects null indices buffer");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullIndicesBuffer,
               "null indices buffer reports null_indices_buffer reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, nullptr, reason),
               "tryCanScheduleProbeUpdates rejects null count output");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullCountOutput,
               "null count output reports null_count_output reason");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleReject returns None for valid buffers");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleReject(0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "classifyProbeScheduleReject returns ZeroProbeCount");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "wouldSkipProbeSchedule false for valid buffers");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, &count),
               "wouldSkipProbeSchedule true for zero probe count");

    indices[0] = 99u;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdates succeeds on valid preflight");
    expectTrue(count == 64u && indices[0] == 0u,
               "tryScheduleProbeUpdates writes round-robin indices");

    indices[0] = 42u;
    const fuse::u32 staleIndex = indices[0];
    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdates rejects zero probe count");
    expectTrue(indices[0] == staleIndex, "tryScheduleProbeUpdates leaves indices unchanged on reject");
    expectTrue(count == 0u, "tryScheduleProbeUpdates zeroes count on reject");
}

void testClassifyWouldSkipGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for classify/wouldSkip test");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, coords) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classifyProbeSampleCoordsReject returns None for valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, coords),
               "wouldSkipProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeSampleCoords reversed = coords;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, reversed) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "classifyProbeSampleCoordsReject returns UnorderedCorners");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkipProbeSampleCoords true for unordered corners");

    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classifyCacheIndexReject returns None for valid index");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndex(desc, 3u, 8u),
               "wouldSkipCacheIndex false for valid index");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "classifyCacheIndexReject returns OutOfRangeProbeIndex");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndex(desc, 99u, 8u),
               "wouldSkipCacheIndex true for OOB index");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, cache.data(), 3u, 8u, cacheReason),
               "tryValidateCacheIndex with cache pointer succeeds");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, nullptr, 3u, 8u, cacheReason),
               "tryValidateCacheIndex rejects null cache pointer");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache pointer reports NullCache reason");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::NullCache,
               "classifyCacheIndexReject with null cache returns NullCache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndex(desc, nullptr, 3u, 8u),
               "wouldSkipCacheIndex true for null cache");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject returns None for accessible grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for accessible grid");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject returns NullCache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");

    fuse::u32 validIndices[2] = {0u, 7u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, validIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "classifyProbeUpdateLaunchReject returns None for valid launch");
    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, oobIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "classifyProbeUpdateLaunchReject returns OutOfRangeProbeIndex");
}

void testClassifyProbeGuardRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for classify test");

    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classify sample coords returns none for valid coords");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, reversed) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "classify sample coords returns unordered_corners");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(empty, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "classify sample coords returns empty_grid");

    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classify cache index returns none for valid index");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "classify cache index returns out_of_range_probe_index");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::NullCache,
               "classify cache index returns null_cache");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classify trilinear sample returns none for valid inputs");
    expectTrue(fuse::renderer::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classify trilinear sample returns null_cache");
    expectTrue(fuse::renderer::classifyProbeTrilinearSampleReject(desc, reversed, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classify trilinear sample returns invalid_sample_coords");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classify schedule returns none for valid inputs");
    expectTrue(fuse::renderer::classifyProbeScheduleReject(0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "classify schedule returns zero_probe_count");
    expectTrue(fuse::renderer::classifyProbeScheduleReject(2048u, 64u, nullptr, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::NullOutIndices,
               "classify schedule returns null_out_indices");

    fuse::u32 validLaunch[2] = {0u, 7u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, validLaunch, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "classify launch returns none for valid indices");
    fuse::u32 oobLaunch[2] = {0u, 99u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, oobLaunch, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "classify launch returns out_of_range_probe_index");

    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validLaunch;
    kernelParams.probe_update_count = 2u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classify kernel returns none for valid params");
    kernelParams.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classify kernel returns zero_rays_per_probe");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCanSampleAtProbeCoords(desc, built, cache.data(), 8u),
               "wouldSkipCanSampleAtProbeCoords false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCanSampleAtProbeCoords(desc, built, nullptr, 8u),
               "wouldSkipCanSampleAtProbeCoords true for null cache");
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(kernelParams),
               "wouldSkipProbeTraceKernel true for zero rays per probe");
    kernelParams.rays_per_probe = 256u;
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(kernelParams),
               "wouldSkipProbeTraceKernel false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(kernelParams),
               "wouldSkipProbeBlendKernel false for valid params");
}

void testWouldSkipGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for wouldSkip sample-coord test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, coords),
               "wouldSkip false for valid sample coords");

    fuse::renderer::ProbeSampleCoords reversed = coords;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered sample coords");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(empty, coords),
               "wouldSkip true for sample coords on empty grid");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    cache[3u].irradiance = {0.25f, 0.5f, 0.75f};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 8u, 3u),
               "wouldSkip false for readable cache index");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, nullptr, 8u, 3u),
               "wouldSkip true for null cache read");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 4u, 3u),
               "wouldSkip true when cache undersized for full grid read");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(desc, coords, cache.data(), 8u),
               "wouldSkip false for valid trilinear sample preflight");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(desc, coords, nullptr, 8u),
               "wouldSkip true for null cache trilinear preflight");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(desc, reversed, cache.data(), 8u),
               "wouldSkip true for invalid coords trilinear preflight");

    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip false for valid trace kernel params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip false for valid blend kernel params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkip true for zero trace update count");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroCount),
               "wouldSkip true for zero blend update count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(nullIndices),
               "wouldSkip true for null trace probe indices");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip true for null blend probe indices");
}

void testTryLaunchProbeKernels() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_trace_kernel succeeds for valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful trace tryLaunch reports no reject reason");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_blend_kernel succeeds for valid params");

    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelReject returns None for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkipProbeTraceKernel false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkipProbeBlendKernel false for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch_probe_trace_kernel rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "tryLaunch trace reports zero_update_count reason");
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(zeroCount) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "classifyProbeKernelReject returns ZeroUpdateCount");
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkipProbeTraceKernel true for zero update count");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroCount),
               "wouldSkipProbeBlendKernel true for zero update count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(nullIndices, nullptr, reason),
               "tryLaunch_probe_blend_kernel rejects null probe indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "tryLaunch blend reports null_probe_indices reason");
}

void testProbeSampleCoordPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for preflight test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, built, reason),
               "tryPreflightProbeSampleCoords succeeds for valid coords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "valid preflight reports no reject reason");
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "canPreflightProbeSampleCoords true for valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "wouldClampProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeSampleCoords warnWeights = built;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, warnWeights, reason),
               "tryPreflightProbeSampleCoords warns but succeeds for clampable weights");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "clampable weights report out_of_range_weights reason");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, warnWeights),
               "wouldClampProbeSampleCoords true for OOB weights");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, reversed, reason),
               "tryPreflightProbeSampleCoords warns but succeeds for unordered corners");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "unordered corners report unordered_corners reason");

    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, hardOob, reason),
               "tryPreflightProbeSampleCoords rejects hard OOB corner indices");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "hard OOB indices report out_of_range_indices reason");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, hardOob),
               "wouldClampProbeSampleCoords true for hard OOB indices");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(empty, built, reason),
               "tryPreflightProbeSampleCoords rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid preflight reports empty_grid reason");
}

void testProbeSchedulePreflight() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates succeeds for valid inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule reports no reject reason");
    expectTrue(count == 4u, "tryScheduleProbeUpdates schedules expected count");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(16u, 8u, indices, &count),
               "wouldSkipProbeSchedule false for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 4u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_probe_count") == 0,
               "zero_probe_count schedule reject reason label");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 8u, indices, &count),
               "wouldSkipProbeSchedule true for zero probe count");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, nullptr, 8u, &count, reason),
               "tryScheduleProbeUpdates rejects null output indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputIndices,
               "null output indices report null_output_indices reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, indices, 8u, nullptr, reason),
               "tryScheduleProbeUpdates rejects null output count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputCount,
               "null output count reports null_output_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, indices, 0u, &count, reason),
               "tryScheduleProbeUpdates rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(16u, 0u, indices, &count),
               "wouldSkipProbeSchedule true for zero max indices");
}

void testCacheIndexPointerGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, cache.data(), 3u, 8u, reason),
               "pointer overload succeeds for valid cache index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid pointer cache index reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, nullptr, 3u, 8u, reason),
               "pointer overload rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache reject reason label");

    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndex(desc, 99u, 8u),
               "wouldClampCacheIndex true for OOB probe index");
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndex(desc, 3u, 4u),
               "wouldClampCacheIndex true for undersized cache");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndex(desc, 3u, 8u),
               "wouldClampCacheIndex false for valid index and cache");
}

void testLaunchZeroRaysPerProbe() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.rays_per_probe = 0u;

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason reason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "tryCanLaunch rejects zero rays per probe");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::ZeroRaysPerProbe,
               "zero rays per probe reports zero_rays_per_probe launch reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe launch reject reason label");
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(desc, validIndices, 2u, nullptr),
               "launch rejects zero rays per probe");
}

void testProbeKernelResourcePreflight() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.probe_world_positions = reinterpret_cast<const void*>(0x1000);
    validParams.prev_irradiance_surface = reinterpret_cast<void*>(0x2000);
    validParams.out_radiance_surface = reinterpret_cast<void*>(0x3000);
    validParams.irradiance_atlas_surface = reinterpret_cast<void*>(0x4000);
    validParams.depth_atlas_surface = reinterpret_cast<void*>(0x5000);

    fuse::renderer::gi::ProbeKernelResourceRejectReason resourceReason =
        fuse::renderer::gi::ProbeKernelResourceRejectReason::None;
    expectTrue(fuse::renderer::gi::tryValidateProbeKernelResources(validParams, resourceReason),
               "valid kernel params pass resource preflight");
    expectTrue(resourceReason == fuse::renderer::gi::ProbeKernelResourceRejectReason::None,
               "valid resource params report no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelResourceRejectReasonLabel(resourceReason), "none") == 0,
               "none kernel resource reject reason label");
    expectTrue(fuse::renderer::gi::hasProbeTraceGpuResources(validParams),
               "hasProbeTraceGpuResources true when all pointers set");
    expectTrue(fuse::renderer::gi::hasProbeBlendGpuResources(validParams),
               "hasProbeBlendGpuResources true when all pointers set");

    fuse::renderer::gi::ProbeKernelRejectReason launchReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernelWithResources(
                   validParams, launchReason, resourceReason),
               "tryCanLaunchProbeTraceKernelWithResources succeeds for full params");
    expectTrue(launchReason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "full trace launch reports no launch reject reason");
    expectTrue(resourceReason == fuse::renderer::gi::ProbeKernelResourceRejectReason::None,
               "full trace launch reports no resource reject reason");

    fuse::renderer::gi::DDGIKernelParams stubParams{};
    stubParams.probe_indices_to_update = indices;
    stubParams.probe_update_count = 2u;
    expectTrue(!fuse::renderer::gi::tryValidateProbeKernelResources(stubParams, resourceReason),
               "stub params fail resource preflight");
    expectTrue(resourceReason == fuse::renderer::gi::ProbeKernelResourceRejectReason::NullProbeWorldPositions,
               "stub params report null_probe_world_positions reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelResourceRejectReasonLabel(resourceReason),
                           "null_probe_world_positions") == 0,
               "null_probe_world_positions resource reject reason label");
    expectTrue(!fuse::renderer::gi::hasProbeTraceGpuResources(stubParams),
               "hasProbeTraceGpuResources false for stub params");

    fuse::renderer::gi::DDGIKernelParams nullAtlas = validParams;
    nullAtlas.irradiance_atlas_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryValidateProbeKernelResources(nullAtlas, resourceReason),
               "null irradiance atlas fails resource preflight");
    expectTrue(resourceReason == fuse::renderer::gi::ProbeKernelResourceRejectReason::NullIrradianceAtlas,
               "null irradiance atlas reports null_irradiance_atlas reason");

    fuse::renderer::gi::DDGIKernelParams nullDepth = validParams;
    nullDepth.depth_atlas_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryValidateProbeKernelResources(nullDepth, resourceReason),
               "null depth atlas fails resource preflight");
    expectTrue(resourceReason == fuse::renderer::gi::ProbeKernelResourceRejectReason::NullDepthAtlas,
               "null depth atlas reports null_depth_atlas reason");

    fuse::renderer::gi::DDGIKernelParams nullPrev = validParams;
    nullPrev.prev_irradiance_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryValidateProbeKernelResources(nullPrev, resourceReason),
               "null prev irradiance surface fails resource preflight");
    expectTrue(resourceReason == fuse::renderer::gi::ProbeKernelResourceRejectReason::NullPrevIrradianceSurface,
               "null prev irradiance reports null_prev_irradiance_surface reason");

    fuse::renderer::gi::DDGIKernelParams nullOut = validParams;
    nullOut.out_radiance_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryValidateProbeKernelResources(nullOut, resourceReason),
               "null out radiance surface fails resource preflight");
    expectTrue(resourceReason == fuse::renderer::gi::ProbeKernelResourceRejectReason::NullOutRadianceSurface,
               "null out radiance reports null_out_radiance_surface reason");

    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernelWithResources(
                   stubParams, launchReason, resourceReason),
               "tryCanLaunchProbeTraceKernelWithResources fails without resources");
    expectTrue(resourceReason == fuse::renderer::gi::ProbeKernelResourceRejectReason::NullProbeWorldPositions,
               "combined preflight reports resource reject reason");
}

void testProbeSchedulePreflights() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates succeeds for valid inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None, "valid schedule reports no reject reason");
    expectTrue(count == 8u, "tryScheduleProbeUpdates writes scheduled count");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");

    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 8u, indices, &count),
               "canScheduleProbeUpdates accepts valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 8u, indices, &count),
               "wouldSkipProbeSchedule false for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, nullptr, 8u, &count, reason),
               "tryScheduleProbeUpdates rejects null output indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputIndices,
               "null output indices report null_output_indices reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "null_output_indices") == 0,
               "null_output_indices schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 8u, nullptr, reason),
               "tryScheduleProbeUpdates rejects null output count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputCount,
               "null output count reports null_output_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 64u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 0u, &count, reason),
               "tryScheduleProbeUpdates rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 8u, indices, &count),
               "wouldSkipProbeSchedule true for zero probe count");
}

void testCacheIndexNullCache() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, nullptr, 3u, 8u, reason),
               "null cache fails cache-index validation");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache cache-index reject reason label");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, cache.data(), 3u, 8u, reason),
               "non-null cache passes cache-index validation");
    expectTrue(fuse::renderer::ddgi_util::isCacheIndexValid(desc, cache.data(), 3u, 8u),
               "isCacheIndexValid with cache pointer accepts valid index");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValid(desc, nullptr, 3u, 8u),
               "isCacheIndexValid with null cache rejected");
}

void testSampleRequestRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::DDGISampleRequest request{};
    request.world_position = {0.5f, 0.5f, 0.5f};
    request.world_normal = {0.f, 1.f, 0.f};

    fuse::renderer::SampleRequestRejectReason reason = fuse::renderer::SampleRequestRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateSampleRequest(desc, request, 8u, reason),
               "valid sample request passes tryValidate");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::None, "valid sample request reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleRequestRejectReasonLabel(reason), "none") == 0,
               "none sample-request reject reason label");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipSampleRequest(desc, request, 8u),
               "wouldSkipSampleRequest false for valid request");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(desc, request, 4u, reason),
               "undersized cache fails sample-request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache sample-request reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipSampleRequest(desc, request, 4u),
               "wouldSkipSampleRequest true for undersized cache");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(empty, request, 8u, reason),
               "empty grid fails sample-request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample-request reason");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(zeroRes, request, 8u, reason),
               "non-sampleable grid fails sample-request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::NotSampleable,
               "zero irradiance_res reports not_sampleable sample-request reason");
}

void testWouldSkipProbeSampleCoords() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldSkip test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered corners");
}

void testProbeKernelWouldSkipAndTryLaunch() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkipProbeTraceKernel false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkipProbeBlendKernel false for valid params");

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_trace_kernel succeeds for valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful trace tryLaunch reports no reject reason");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_blend_kernel succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkipProbeTraceKernel true for zero update count");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch_probe_trace_kernel rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "tryLaunch trace reports zero_update_count reason");
}

void testProbeSampleCoordPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for preflight test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, built, reason),
               "tryPreflightProbeSampleCoords succeeds for valid coords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "valid preflight reports no reject reason");
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "canPreflightProbeSampleCoords succeeds for valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "wouldClampProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeSampleCoords warnWeights = built;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, warnWeights, reason),
               "tryPreflightProbeSampleCoords warns but succeeds for clampable weights");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "clampable weights report out_of_range_weights reason");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, warnWeights),
               "wouldClampProbeSampleCoords true for clampable weights");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, reversed, reason),
               "tryPreflightProbeSampleCoords warns but succeeds for unordered corners");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "unordered corners report unordered_corners reason");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, reversed),
               "wouldClampProbeSampleCoords true for unordered corners");

    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, hardOob, reason),
               "tryPreflightProbeSampleCoords rejects hard OOB indices");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "hard OOB indices report out_of_range_indices reason");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, hardOob),
               "wouldClampProbeSampleCoords true for hard OOB indices");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(empty, built, reason),
               "tryPreflightProbeSampleCoords rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid preflight reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid preflight reject reason label");
}

void testCacheIndexLookupGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);

    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndexLookup(desc, cache.data(), 8u, 3u, reason),
               "tryValidateCacheIndexLookup succeeds for in-range index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache lookup reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndexLookup(desc, nullptr, 8u, 3u, reason),
               "tryValidateCacheIndexLookup rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache reject reason label");

    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndex(desc, 99u),
               "wouldClampCacheIndex true for OOB probe index");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndex(desc, 7u),
               "wouldClampCacheIndex false for last valid probe index");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndex(empty, 0u),
               "wouldClampCacheIndex false on empty grid");
}

void testProbeSchedulePreflightGuards() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, &count, reason),
               "tryCanScheduleProbeUpdates succeeds for valid inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule preflight reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");
    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 64u, indices, &count),
               "canScheduleProbeUpdates succeeds for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "wouldSkipProbeSchedule false for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(0u, 64u, indices, &count, reason),
               "tryCanScheduleProbeUpdates rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, &count),
               "wouldSkipProbeSchedule true for zero probe count");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, nullptr, &count, reason),
               "tryCanScheduleProbeUpdates rejects null output indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputIndices,
               "null output indices report null_output_indices reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, nullptr, reason),
               "tryCanScheduleProbeUpdates rejects null output count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputCount,
               "null output count reports null_output_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 0u, indices, &count, reason),
               "tryCanScheduleProbeUpdates rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices report zero_max_indices reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_max_indices") == 0,
               "zero_max_indices schedule reject reason label");
}

void testProbeKernelTryLaunchGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchDdgiKernelParams(validParams, reason),
               "tryCanLaunchDdgiKernelParams succeeds for valid params");
    expectTrue(fuse::renderer::gi::canLaunchDdgiKernelParams(validParams),
               "canLaunchDdgiKernelParams succeeds for valid params");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_trace_kernel succeeds for valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful tryLaunch trace reports no reject reason");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_blend_kernel succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch_probe_trace_kernel rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "tryLaunch trace zero count reports zero_update_count reason");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(zeroCount, nullptr, reason),
               "tryLaunch_probe_blend_kernel rejects zero update count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(nullIndices, nullptr, reason),
               "tryLaunch_probe_blend_kernel rejects null probe indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "tryLaunch blend null indices report null_probe_indices reason");
}

void testTryCanSampleAtProbeCoordsSoftPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for soft preflight wiring test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;

    fuse::renderer::ProbeSampleCoords softWeights = coords;
    softWeights.tx = 2.f;
    expectTrue(fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, softWeights, cache.data(), 8u, reason),
               "tryCanSampleAtProbeCoords succeeds for clampable weights via preflight path");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "clampable weights report no trilinear reject reason");
}

void testProbeSchedulePreflightGuards() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(
                   0u, 2048u, 64u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates succeeds on valid inputs");
    expectTrue(count == 8u, "tryScheduleProbeUpdates writes scheduled count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");

    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 8u, indices, &count),
               "canScheduleProbeUpdates true for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 8u, indices, &count),
               "wouldSkipProbeSchedule false for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(
                   0u, 0u, 64u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_probe_count") == 0,
               "zero_probe_count schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeSchedule(2048u, 0u, indices, &count, reason),
               "tryValidateProbeSchedule rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeSchedule(2048u, 8u, nullptr, &count, reason),
               "tryValidateProbeSchedule rejects null output indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputIndices,
               "null output indices reports null_output_indices reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "null_output_indices") == 0,
               "null_output_indices schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeSchedule(2048u, 8u, indices, nullptr, reason),
               "tryValidateProbeSchedule rejects null output count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputCount,
               "null output count reports null_output_count reason");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 8u, indices, &count),
               "wouldSkipProbeSchedule true for zero probe count");
}

void testCacheIndexNullCacheGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, 3u, nullptr, 8u, reason),
               "null cache fails cache-index validation");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache cache-index reject reason label");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    cache[3u].irradiance = {0.5f, 0.25f, 0.125f};
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, 3u, cache.data(), 8u, reason),
               "non-null cache passes cache-index validation");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache-index reports no reject reason");

    expectTrue(fuse::renderer::ddgi_util::isCacheIndexValid(desc, 3u, cache.data(), 8u),
               "isCacheIndexValid true for accessible cache");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValid(desc, 3u, nullptr, 8u),
               "isCacheIndexValid false for null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, nullptr, 8u),
               "wouldSkipCacheIndexLookup true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, cache.data(), 8u),
               "wouldSkipCacheIndexLookup false for accessible cache");

    fuse::math::Vec3 irradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, cache.data(), 8u, 3u, irradiance, reason),
               "tryRead with reason succeeds for valid index");
    expectNear(irradiance.x, 0.5f, 1e-5f, "tryRead with reason returns stored irradiance");
    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, nullptr, 8u, 3u, irradiance, reason),
               "tryRead with reason rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "tryRead with reason reports null_cache");
}

void testWouldSkipProbeSampleCoords() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldSkip test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered corners");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(empty, built),
               "wouldSkip true on empty grid");
}

void testProbeKernelTryLaunchGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch trace succeeds with valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful trace tryLaunch reports no reject reason");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch blend succeeds with valid params");

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkipProbeTraceKernel false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkipProbeBlendKernel false for valid params");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(nullIndices),
               "wouldSkipProbeTraceKernel true for null indices");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkipProbeBlendKernel true for null indices");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(nullIndices, nullptr, reason),
               "tryLaunch trace rejects null indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "tryLaunch trace null indices reports null_probe_indices reason");
}

void testProbeKernelResourcePreflights() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::f32 world_positions[6] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f};
    fuse::u8 irradiance_atlas[16]{};
    fuse::u8 depth_atlas[16]{};

    fuse::renderer::gi::DDGIKernelParams params{};
    params.probe_indices_to_update = indices;
    params.probe_update_count = 2u;
    params.probe_world_positions = world_positions;
    params.irradiance_atlas_surface = irradiance_atlas;
    params.depth_atlas_surface = depth_atlas;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryPreflightProbeTraceKernelResources(params, reason),
               "trace resource preflight accepts world positions");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "valid trace resources report no reject reason");

    fuse::renderer::gi::DDGIKernelParams nullWorld = params;
    nullWorld.probe_world_positions = nullptr;
    expectTrue(!fuse::renderer::gi::tryPreflightProbeTraceKernelResources(nullWorld, reason),
               "trace resource preflight rejects null world positions");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeWorldPositions,
               "null world positions report null_probe_world_positions reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason),
                           "null_probe_world_positions") == 0,
               "null_probe_world_positions kernel reject reason label");

    expectTrue(fuse::renderer::gi::tryPreflightProbeBlendKernelResources(params, reason),
               "blend resource preflight accepts atlas surfaces");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "valid blend resources report no reject reason");

    fuse::renderer::gi::DDGIKernelParams nullIrradiance = params;
    nullIrradiance.irradiance_atlas_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryPreflightProbeBlendKernelResources(nullIrradiance, reason),
               "blend resource preflight rejects null irradiance atlas");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullIrradianceAtlas,
               "null irradiance atlas reports null_irradiance_atlas reason");

    fuse::renderer::gi::DDGIKernelParams nullDepth = params;
    nullDepth.depth_atlas_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryPreflightProbeBlendKernelResources(nullDepth, reason),
               "blend resource preflight rejects null depth atlas");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullDepthAtlas,
               "null depth atlas reports null_depth_atlas reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "null_depth_atlas") == 0,
               "null_depth_atlas kernel reject reason label");
}

void testProbeKernelLaunchGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch trace succeeds for valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful trace tryLaunch reports no reject reason");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch blend succeeds for valid params");
               "successful blend tryLaunch reports no reject reason");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_kernels(validParams, nullptr, reason),
               "tryLaunch combined kernels succeeds for valid params");
               "successful combined tryLaunch reports no reject reason");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch trace rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "tryLaunch trace zero count reports zero_update_count reason");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(zeroCount, nullptr, reason),
               "tryLaunch blend rejects zero update count");
               "tryLaunch blend zero count reports zero_update_count reason");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_kernels(zeroCount, nullptr, reason),
               "tryLaunch combined kernels rejects zero update count");
               "tryLaunch combined kernels zero count reports zero_update_count reason");

void testWouldSkipProbeKernels() {

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip false for valid trace kernel params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip false for valid blend kernel params");

    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkip true for zero update count trace");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroCount),
               "wouldSkip true for zero update count blend");

void testClassifyAndSkipGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for classify guard test");

    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, coords) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classify sample coords passes for valid coords");
    expectTrue(!fuse::renderer::shouldSkipProbeSampleCoords(desc, coords),
               "shouldSkip false for valid sample coords");

    fuse::renderer::ProbeSampleCoords reversed = coords;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, reversed) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "classify sample coords reports unordered corners");
    expectTrue(fuse::renderer::shouldSkipProbeSampleCoords(desc, reversed),
               "shouldSkip true for unordered corners");

    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classify cache index passes for valid index");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "classify cache index reports OOB probe index");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::NullCache,
               "classify cache index reports null cache");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classify trilinear sample passes for accessible grid");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "shouldSkip false for valid trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "shouldSkip true for null cache trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 4u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::UndersizedCache,
               "classify trilinear sample reports undersized cache");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classify schedule passes for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleReject(0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "classify schedule reports zero probe count");

    fuse::u32 validIndices[2] = {0u, 7u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, validIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "classify launch passes for valid indices");
    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, oobIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "classify launch reports OOB probe index");

    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classify kernel passes for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(kernelParams),
               "wouldSkip false for valid trace kernel params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(kernelParams),
               "wouldSkip false for valid blend kernel params");

    fuse::renderer::gi::DDGIKernelParams nullIndices = kernelParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(nullIndices) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "classify kernel reports null probe indices");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip true for null probe indices");
}

void testProbeCoordOutOfRange() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {4, 2, 3};

    const fuse::renderer::ProbeGridCoord valid{1, 0, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, valid),
               "in-range probe coord not out of range");

    const fuse::renderer::ProbeGridCoord invalid{4, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, invalid),
               "OOB probe coord flagged out of range");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(empty, valid),
               "any coord out of range on empty grid");
}

void testWouldClampProbeSampleCoords() {
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldClamp test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "valid built coords would not clamp");
void testClassifyAndWouldClampGuards() {
    desc.irradiance_res = 8;

               "build coords for classify guard test");
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classify sample coords reports none for valid coords");
               "wouldClamp false for valid sample coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, oobIndices),
               "OOB indices would clamp");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, oobWeights),
               "OOB weights would clamp");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, reversed),
               "unordered corners would clamp");

void testPreflightProbeSampleCoords() {

               "build coords for preflight test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "canPreflight succeeds for valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, built, reason),
               "tryPreflight succeeds for valid coords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "valid preflight reports no reject reason");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, reversed, reason),
               "tryPreflight rejects unordered corners");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "preflight unordered corners report correct reason");

void testCanLookupAtProbeIndex() {
void testClassifyProbeSampleCoordsReject() {

               "build coords for classify test");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, built) ==
               "valid coords classify to none");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, reversed) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "reversed corners classify to unordered_corners");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered corners");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(empty, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid classifies to empty_grid");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(empty, built),
               "wouldSkip true on empty grid");

void testClassifyCacheIndexReject() {

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    cache[3u].irradiance = {0.25f, 0.5f, 0.75f};

    expectTrue(fuse::renderer::ddgi_util::canLookupAtProbeIndex(desc, cache.data(), 3u, 8u),
               "canLookupAtProbeIndex succeeds for in-range index");
    expectTrue(!fuse::renderer::ddgi_util::canLookupAtProbeIndex(desc, nullptr, 3u, 8u),
               "canLookupAtProbeIndex rejects null cache");
    expectTrue(!fuse::renderer::ddgi_util::canLookupAtProbeIndex(desc, cache.data(), 99u, 8u),
               "canLookupAtProbeIndex rejects OOB index");
    expectTrue(!fuse::renderer::ddgi_util::canLookupAtProbeIndex(desc, cache.data(), 3u, 4u),
               "canLookupAtProbeIndex rejects undersized cache");

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    fuse::math::Vec3 irradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryCanLookupAtProbeIndex(desc, cache.data(), 3u, 8u, reason),
               "tryCanLookupAtProbeIndex succeeds for valid index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid lookup reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, cache.data(), 8u, 3u, irradiance, reason),
               "tryRead with reason succeeds for valid index");
    expectNear(irradiance.x, 0.25f, 1e-5f, "tryRead with reason returns stored irradiance");

    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(99u, desc),
               "OOB probe index would clamp");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(3u, desc),
               "in-range probe index would not clamp");

    expectTrue(!fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, 3u, 8u),
               "shouldSkip false for valid cache-index lookup");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u),
               "shouldSkip true for null cache lookup");

void testShouldSkipProbeSchedule() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(!fuse::renderer::ddgi_util::shouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "shouldSkip false for valid schedule inputs");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeSchedule(0u, 64u, indices, &count),
               "shouldSkip true for zero probe count");

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(0u, 2048u, 0u, 64u, indices, &count, reason),
               "full schedule preflight rejects zero probes per frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes per frame reports zero_probes_per_frame reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_probes_per_frame") == 0,
               "zero_probes_per_frame schedule reject reason label");
    expectTrue(fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(0u, 2048u, 64u, 64u, indices, &count, reason),
               "full schedule preflight succeeds for valid inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid full schedule preflight reports no reject reason");

void testWouldSkipProbeKernels() {
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, cache.data(), 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "valid cache index classifies to none");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 8u, 3u),
               "wouldSkipRead false for valid cache read");

    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache classifies to null_cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, nullptr, 8u, 3u),
               "wouldSkipRead true for null cache");

    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, cache.data(), 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB index classifies to out_of_range_probe_index");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 8u, 99u),
               "wouldSkipRead true for OOB probe index");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 4u, 3u),
               "wouldSkipRead true when cache undersized for grid");

void testValidateScheduledProbeIndices() {

    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(
                   0u, 8u, 4u, indices, 64u, &count, scheduleReason),
               "schedule probes for validation test");
    expectTrue(fuse::renderer::ddgi_util::tryValidateScheduledProbeIndices(desc, indices, count, launchReason),
               "scheduled indices pass validation");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "valid scheduled indices report no reject reason");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateScheduledProbeIndices(desc, oobIndices, 2u, launchReason),
               "OOB scheduled indices fail validation");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "OOB scheduled indices report out_of_range_probe_index");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateScheduledProbeIndices(desc, nullptr, 2u, launchReason),
               "null scheduled indices fail validation");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::NullIndices,
               "null scheduled indices report null_indices reason");

void testWouldSkipProbeTrilinearSample() {

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear skip test");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for accessible grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 4u),
               "wouldSkipProbeTrilinearSample true for undersized cache");

void testHostLaunchZeroRaysPerProbe() {
    desc.rays_per_probe = 0u;

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason reason =
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "host launch preflight rejects zero rays per probe");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::ZeroRaysPerProbe,
               "zero rays per probe reports zero_rays_per_probe launch reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe launch reject reason label");
    expectTrue(fuse::renderer::wouldSkipDdgiProbeUpdate(desc, validIndices, 2u),
               "wouldSkip true for zero rays per probe");

    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip trace false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip blend false for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkip trace true for zero update count");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroCount),
               "wouldSkip blend true for zero update count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(nullIndices),
               "wouldSkip trace true for null probe indices");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip blend true for null probe indices");
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, oobIndices) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "classify sample coords reports out_of_range_indices");
               "wouldClamp true for OOB sample coord indices");

    const fuse::renderer::ProbeGridCoord validCoord{1, 1, 1};
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, validCoord),
               "interior coord not out of range");
    const fuse::renderer::ProbeGridCoord oobCoord{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, oobCoord),

    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 3u, 8u) ==
               "classify cache index reports none for valid index");
               "classify cache index with pointer reports none for valid index");
               "classify cache index reports null_cache");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(desc, 3u),
               "wouldClamp false for in-range cache index");
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(desc, 99u),
               "wouldClamp true for OOB cache index");

    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, cache.data(), 8u, 3u, irradiance,
                                                                   cacheReason),
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "tryRead with reason reports none on success");
    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, nullptr, 8u, 3u, irradiance,
               "tryRead with reason rejects null cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "tryRead with reason reports null_cache");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classify trilinear sample reports none for valid inputs");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classify schedule reports none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleReject(0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "classify schedule reports zero_probe_count");
void testDdgiClassifyPreflightGuards() {

               "build coords for classify/preflight deepen test");

    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, coords) ==
               "classifyProbeSampleCoordsReject none on valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(desc, coords),
               "preflightProbeSampleCoords passes on valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::shouldSkipProbeSampleCoords(desc, coords),
               "shouldSkipProbeSampleCoords false on valid coords");

    fuse::renderer::ProbeSampleCoords reversed = coords;
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, reversed) ==
               "classifyProbeSampleCoordsReject unordered corners");
    fuse::renderer::ProbeSampleCoordsRejectReason sampleReason =
    expectTrue(!fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(desc, reversed, &sampleReason),
               "preflightProbeSampleCoords rejects unordered corners");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "preflightProbeSampleCoords reports unordered_corners");

    fuse::renderer::ProbeSampleCoords builtIfReady{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoordsIfReady(desc, {0.5f, 0.5f, 0.5f},
                                                                              builtIfReady),
               "buildProbeSampleCoordsIfReady succeeds on interior sample");
    fuse::renderer::ProbeSampleCoords unchanged{};
    unchanged.x0 = 7u;
    expectTrue(!fuse::renderer::ProbeGridLayout::buildProbeSampleCoordsIfReady(empty, {0.f, 0.f, 0.f},
                                                                               unchanged),
               "buildProbeSampleCoordsIfReady rejects empty grid");
    expectTrue(unchanged.x0 == 7u, "buildProbeSampleCoordsIfReady leaves coords unchanged on reject");

    cache[3u].irradiance = {0.5f, 0.25f, 0.125f};
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, 3u, 8u) ==
               "classifyCacheIndexReject none on valid index");
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndex(desc, 3u, 8u),
               "preflightCacheIndex passes on valid index");
    expectTrue(fuse::renderer::ddgi_util::cacheIndexLookupReady(desc, 3u, 8u),
               "cacheIndexLookupReady true on valid index");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, 8u),
               "wouldSkipCacheIndexLookup false on valid index");

    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndex(desc, nullptr, 3u, 8u, &cacheReason),
               "preflightCacheIndex rejects null cache");
               "preflightCacheIndex reports null_cache");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
               "classifyCacheIndexReject null cache");

    fuse::math::Vec3 readIrradiance{};
    expectTrue(fuse::renderer::ddgi_util::readIrradianceAtIndexIfReady(desc, cache.data(), 8u, 3u, readIrradiance),
               "readIrradianceAtIndexIfReady succeeds on valid index");
    expectNear(readIrradiance.x, 0.5f, 1e-5f, "readIrradianceAtIndexIfReady returns stored irradiance");

    expectTrue(fuse::renderer::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject none on accessible grid");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample passes on accessible grid");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "shouldSkipTrilinearProbeSample false on accessible grid");

    fuse::math::Vec3 trilinearSample{};
    expectTrue(fuse::renderer::ddgi_util::trilinearProbeIrradianceIfReady(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, trilinearSample),
               "trilinearProbeIrradianceIfReady succeeds on full cache");
    expectTrue(trilinearSample.x >= 0.f, "trilinearProbeIrradianceIfReady returns non-negative sample");

    expectTrue(fuse::renderer::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
               "classifyProbeScheduleReject none on valid schedule inputs");
    expectTrue(fuse::renderer::ddgi_util::probeScheduleReady(2048u, 64u, indices, &count),
               "probeScheduleReady true on valid schedule inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeSchedule(2048u, 64u, indices, &count),
               "preflightProbeSchedule passes on valid schedule inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "wouldSkipProbeSchedule false on valid schedule inputs");

    expectTrue(fuse::renderer::classifyProbeScheduleReject(0u, 64u, indices, &count) ==
               "classifyProbeScheduleReject zero probe count");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeSchedule(0u, 64u, indices, &count, &scheduleReason),
               "preflightProbeSchedule rejects zero probe count");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "preflightProbeSchedule reports zero_probe_count");

    fuse::u32 validLaunch[2] = {0u, 7u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, validLaunch, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "classify launch reports none for valid indices");
    fuse::u32 oobLaunch[2] = {0u, 99u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, oobLaunch, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "classify launch reports out_of_range_probe_index");
               "classifyProbeUpdateLaunchReject none on valid launch");
    expectTrue(fuse::renderer::preflightDdgiProbeUpdate(desc, validLaunch, 2u),
               "preflightDdgiProbeUpdate passes on valid launch");
    expectTrue(!fuse::renderer::wouldSkipDdgiProbeUpdate(desc, validLaunch, 2u),
               "wouldSkipDdgiProbeUpdate false on valid launch");

    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
               "classifyProbeUpdateLaunchReject OOB probe index");
    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(desc, oobLaunch, 2u, &launchReason),
               "preflightDdgiProbeUpdate rejects OOB indices");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "preflightDdgiProbeUpdate reports out_of_range_probe_index");

    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validLaunch;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classify kernel reports none for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(kernelParams),
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(kernelParams),

    fuse::renderer::gi::DDGIKernelParams nullIndices = kernelParams;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(nullIndices) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "classify kernel reports null_probe_indices");
               "wouldSkip trace true for null indices");
               "wouldSkip blend true for null indices");
void testClassifyGuardHelpers() {

    fuse::renderer::ProbeSampleCoords valid{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, valid),
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, valid) ==
               "classify valid sample coords reports none");
    expectTrue(!fuse::renderer::wouldClampProbeSampleCoords(desc, valid),
               "valid sample coords would not clamp");

    fuse::renderer::ProbeSampleCoords oob = valid;
    oob.x0 = 9u;
    oob.x1 = 9u;
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, oob) ==
               "classify OOB indices reports out_of_range_indices");
    expectTrue(fuse::renderer::wouldClampProbeSampleCoords(desc, oob),
               "OOB sample coords would clamp");

    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, 3u, 8u) ==
               "classify valid cache index reports none");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, cache.data(), 3u, 8u) ==
               "classify valid cache pointer reports none");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
               "classify null cache reports null_cache");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, 8u, cacheReason),
               "wouldSkip with reason false for valid index");
               "valid cache-index wouldSkip reports none reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u, cacheReason),
               "wouldSkip with reason true for null cache");
               "null cache wouldSkip reports null_cache reason");

    expectTrue(fuse::renderer::classifyProbeTrilinearSampleReject(desc, valid, cache.data(), 8u) ==
               "classify valid trilinear sample reports none");
    expectTrue(!fuse::renderer::shouldSkipTrilinearProbeSample(desc, valid, cache.data(), 8u),
               "shouldSkip false for valid trilinear sample");
    expectTrue(fuse::renderer::shouldSkipTrilinearProbeSample(desc, valid, nullptr, 8u),
               "shouldSkip true for null cache trilinear sample");

    expectTrue(fuse::renderer::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
               "classify valid schedule inputs reports none");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count, scheduleReason),
               "wouldSkip schedule false for valid inputs");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule wouldSkip reports none reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, &count, scheduleReason),
               "wouldSkip schedule true for zero probe count");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count wouldSkip reports zero_probe_count reason");

    fuse::u32 launchIndices[2] = {0u, 7u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, launchIndices, 2u) ==
               "classify valid launch reports none");

    expectTrue(!fuse::renderer::wouldSkipDdgiProbeUpdate(desc, launchIndices, 2u, launchReason),
               "wouldSkip launch false for valid indices");
               "valid launch wouldSkip reports none reason");

    expectTrue(fuse::renderer::wouldSkipDdgiProbeUpdate(desc, oobLaunch, 2u, launchReason),
               "wouldSkip launch true for OOB indices");
               "OOB launch wouldSkip reports out_of_range_probe_index reason");

    kernelParams.probe_indices_to_update = launchIndices;
               "classify valid kernel params reports none");
               "wouldSkip trace false for valid kernel params");
               "wouldSkip blend false for valid kernel params");

    kernelParams.rays_per_probe = 0u;
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classify zero rays reports zero_rays_per_probe");
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(kernelParams),
               "wouldSkip trace true for zero rays per probe");

    fuse::renderer::CacheIndexRejectReason readReason = fuse::renderer::CacheIndexRejectReason::None;
    cache[3u].irradiance = {0.1f, 0.2f, 0.3f};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, cache.data(), 8u, 3u, irradiance, readReason),
    expectTrue(readReason == fuse::renderer::CacheIndexRejectReason::None,
               "successful tryRead reports none reason");
    expectNear(irradiance.x, 0.1f, 1e-5f, "tryRead with reason returns stored irradiance");
    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, nullptr, 8u, 3u, irradiance, readReason),
    expectTrue(readReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache tryRead reports null_cache reason");
               "classifyProbeKernelReject none on valid kernel params");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(kernelParams),
               "preflightProbeTraceKernel passes on valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(kernelParams),
               "preflightProbeBlendKernel passes on valid params");
               "wouldSkipProbeTraceKernel false on valid params");
               "wouldSkipProbeBlendKernel false on valid params");

    nullIndices.probe_indices_to_update = nullptr;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
               "classifyProbeKernelReject null probe indices");
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(nullIndices, &kernelReason),
               "preflightProbeTraceKernel rejects null probe indices");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "preflightProbeTraceKernel reports null_probe_indices");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkipProbeBlendKernel true for null probe indices");
}

void testProbeCoordOutOfRangeAndClampCacheLookup() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    const fuse::renderer::ProbeGridCoord valid{1, 1, 1};
    const fuse::renderer::ProbeGridCoord invalid{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeCoord(desc, valid),
               "interior coord is valid");
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, invalid),
               "OOB coord flagged out of range");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, valid),
               "valid coord not out of range");
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(99u, desc),
               "OOB probe index would clamp");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(3u, desc),
               "in-range probe index would not clamp");

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanLookupAtCoord(desc, valid, 8u, reason),
               "coord lookup preflight succeeds on accessible grid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid coord reports no cache lookup reject reason");
    expectTrue(fuse::renderer::ddgi_util::canLookupAtCoord(desc, valid, 8u),
               "canLookupAtCoord true on accessible grid");

    expectTrue(fuse::renderer::ddgi_util::tryCanLookupAtCoord(desc, invalid, 8u, reason),
               "OOB coord lookup preflight still succeeds with warning");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB coord reports out_of_range_probe_index reason");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupAtCoord(empty, valid, 8u, reason),
               "empty grid coord lookup preflight fails");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid coord lookup reports empty_grid reason");
}

void testProbeSampleCoordPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for preflight test");
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "built coords pass canPreflightProbeSampleCoords");

    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, built, reason),
               "built coords pass tryPreflightProbeSampleCoords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "valid coords report no preflight reject reason");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "valid coords would not clamp");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, oobIndices),
               "OOB indices fail canPreflightProbeSampleCoords");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, oobIndices),
               "OOB indices would clamp");
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, oobIndices, reason),
               "OOB indices fail tryPreflightProbeSampleCoords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "OOB indices report out_of_range_indices preflight reason");
}

void testTryReadIrradianceAtCoord() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    cache[3u].irradiance = {0.1f, 0.2f, 0.3f};

    const fuse::renderer::ProbeGridCoord coord{1, 1, 0};
    fuse::math::Vec3 irradiance{};
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtCoord(desc, cache.data(), 8u, coord, irradiance, reason),
               "tryReadIrradianceAtCoord succeeds for valid coord");
    expectNear(irradiance.x, 0.1f, 1e-5f, "coord read returns stored irradiance x");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "successful coord read reports no reject reason");

    const fuse::renderer::ProbeGridCoord invalid{9, 0, 0};
    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtCoord(desc, cache.data(), 8u, invalid, irradiance, reason),
               "tryReadIrradianceAtCoord rejects OOB coord");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB coord read reports out_of_range_probe_index reason");

    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtCoord(desc, nullptr, 8u, coord, irradiance, reason),
               "tryReadIrradianceAtCoord rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache coord read reports null_cache reason");
}

void testTryReadIrradianceAtIndexRejectReason() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    cache[2u].irradiance = {0.4f, 0.5f, 0.6f};

    fuse::math::Vec3 irradiance{};
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, cache.data(), 8u, 2u, irradiance, reason),
               "tryReadIrradianceAtIndex with reason succeeds for valid index");
    expectNear(irradiance.y, 0.5f, 1e-5f, "index read with reason returns stored irradiance y");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid index read reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, cache.data(), 4u, 2u, irradiance, reason),
               "tryReadIrradianceAtIndex with reason rejects undersized cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "undersized cache index read reports undersized_cache reason");
}

void testWouldSkipTrilinearProbeSample() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for wouldSkip trilinear test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkip false for accessible trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkip true for null cache trilinear sample");
    const fuse::math::Vec3 centrePos{0.5f, 0.5f, 0.5f};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, centrePos, cache.data(), 4u),
               "wouldSkip true for undersized cache world-position sample");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    const fuse::math::Vec3 originPos{0.f, 0.f, 0.f};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(empty, originPos, cache.data(), 8u),
               "wouldSkip true for empty grid world-position sample");
}

void testWouldSkipProbeKernels() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip false for valid trace kernel params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip false for valid blend kernel params");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(nullIndices),
               "wouldSkip true for null probe indices trace launch");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip true for null probe indices blend launch");

    fuse::renderer::gi::DDGIKernelParams zeroRays = validParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroRays),
               "wouldSkip true for zero rays per probe trace launch");
}

void testSampleRequestRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::DDGISampleRequest request{};
    request.world_position = {0.5f, 0.5f, 0.5f};
    request.world_normal = {0.f, 1.f, 0.f};

    fuse::renderer::SampleRequestRejectReason reason = fuse::renderer::SampleRequestRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateSampleRequest(desc, request, 8u, reason),
               "tryValidateSampleRequest succeeds for valid request");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::None, "valid sample request reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleRequestRejectReasonLabel(reason), "none") == 0,
               "none sample-request reject reason label");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipDdgiSample(desc, request, 8u),
               "wouldSkipDdgiSample false when cache sized");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipDdgiSample(desc, request, 4u),
               "wouldSkipDdgiSample true when cache undersized");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(empty, request, 8u, reason),
               "empty grid fails sample request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample-request reason");
}

void testCacheIndexClassifyAndReady() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classifyCacheIndexReject none for valid index");
    expectTrue(fuse::renderer::ddgi_util::cacheIndexReady(desc, 3u, 8u),
               "cacheIndexReady true for valid index");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::math::Vec3 irradiance{};
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, cache.data(), 8u, 3u, irradiance, reason),
               "tryRead with reason succeeds for valid index");
    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, nullptr, 8u, 3u, irradiance, reason),
               "tryRead with reason rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "tryRead with reason reports null_cache");
}

void testProbeSampleCoordClassifyAndPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for classify/preflight test");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classifyProbeSampleCoordsReject none for valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(desc, built),
               "preflightProbeSampleCoords succeeds for valid coords");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered corners");
}

void testScheduleClassifyAndValidate() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleReject none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightScheduleProbeUpdates(2048u, 64u, indices, &count),
               "preflightScheduleProbeUpdates succeeds for valid inputs");

    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    const fuse::u32 probeCount = fuse::renderer::ddgi_util::probeCount(desc);

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(
                   0u, probeCount, probeCount, indices, 64u, &count, reason),
               "schedule frame 0 for validate test");
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::validateScheduledProbeIndices(desc, indices, count, launchReason),
               "validateScheduledProbeIndices accepts in-range scheduled indices");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::ddgi_util::validateScheduledProbeIndices(desc, oobIndices, 2u, launchReason),
               "validateScheduledProbeIndices rejects OOB indices");
}

void testKernelWouldSkipAndSurfacePreflight() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkipProbeTraceKernel false for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeKernelTraceReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelTraceReject none for valid params");

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::tryPreflightProbeTraceWorldPositions(validParams, reason),
               "trace world-position preflight rejects null positions");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeWorldPositions,
               "null positions report null_probe_world_positions reason");

    fuse::u8 surfaceByte = 0u;
    void* surfacePtr = &surfaceByte;
    fuse::renderer::gi::DDGIKernelParams surfacedParams = validParams;
    surfacedParams.probe_world_positions = surfacePtr;
    surfacedParams.prev_irradiance_surface = surfacePtr;
    surfacedParams.out_radiance_surface = surfacePtr;
    surfacedParams.irradiance_atlas_surface = surfacePtr;
    surfacedParams.depth_atlas_surface = surfacePtr;
    expectTrue(fuse::renderer::gi::tryPreflightProbeTraceWorldPositions(surfacedParams, reason),
               "trace world-position preflight succeeds with positions");
    expectTrue(fuse::renderer::gi::tryPreflightProbeBlendSurfaces(surfacedParams, reason),
               "blend surface preflight succeeds with all surfaces");
}

void testProbeKernelLaunchGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip trace false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip blend false for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classify returns none for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkip trace true for zero update count");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroCount),
               "wouldSkip blend true for zero update count");
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(zeroCount) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "classify returns zero_update_count for zero count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(nullIndices) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "classify returns null_probe_indices for null buffer");
}

void testProbeKernelLaunchGuards() {

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(validParams, reason),
               "valid kernel params pass trace preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None, "valid trace params report no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "none") == 0,
               "none kernel reject reason label");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkipProbeTraceKernel false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkipProbeBlendKernel false for valid params");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(validParams, nullptr),
               "trace kernel launch succeeds with valid params");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(validParams, nullptr),
               "blend kernel launch succeeds with valid params");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_trace_kernel succeeds with valid params");
    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch_probe_blend_kernel succeeds with valid params");

    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(zeroCount, reason),
               "zero update count fails trace preflight");
               "zero update count reports zero_update_count reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "zero_update_count") == 0,
               "zero_update_count kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkipProbeTraceKernel true for zero update count");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch_probe_trace_kernel rejects zero update count");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(zeroCount, nullptr),
               "trace kernel launch rejects zero update count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(nullIndices, reason),
               "null probe indices fail blend preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "null probe indices report null_probe_indices reason");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkipProbeBlendKernel true for null indices");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(nullIndices, nullptr, reason),
               "tryLaunch_probe_blend_kernel rejects null probe indices");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(nullIndices, nullptr),
               "blend kernel launch rejects null probe indices");

    fuse::renderer::gi::DDGIKernelParams zeroRays = validParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(zeroRays, reason),
               "zero rays per probe fails trace preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "zero rays per probe reports zero_rays_per_probe reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroRays),
               "wouldSkipProbeTraceKernel true for zero rays per probe");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroRays, nullptr, reason),
               "tryLaunch_probe_trace_kernel rejects zero rays per probe");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(zeroRays, nullptr),
               "trace kernel launch rejects zero rays per probe");

void testProbeGridSourceRejectReasons() {

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(desc, reason),
               "sampleable grid passes probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid grid reports no probe-grid source reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(empty, reason),
               "empty grid fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid reports empty_grid probe-grid source reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid probe-grid source reject reason label");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(zeroSpacing) ==
                   fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "zero spacing classifies as not_sampleable");
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightProbeGridSource(zeroSpacing, reason),
               "tryPreflightProbeGridSource rejects non-sampleable grid");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "zero spacing reports not_sampleable probe-grid source reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "not_sampleable") == 0,
               "not_sampleable probe-grid source reject reason label");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");

void testDdgiWouldSkipDeepenGuards() {

               "build coords for wouldSkip deepen test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkip false for valid sample-coord preflight");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}),
               "wouldSkip false for valid world-position sample coords");

    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(empty, {0.f, 0.f, 0.f}),
               "wouldSkip true for sample coords on empty grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");

    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkip true for hard OOB sample-coord preflight");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(empty, built),
               "wouldSkip true for sample-coord preflight on empty grid");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAfterClamp(desc, 999u, 8u),
               "OOB probe index that clamps does not skip cache lookup");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 999u, 8u),
               "strict cache-index skip still rejects OOB probe index");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAfterClamp(desc, nullptr, 3u, 8u),
               "wouldSkipAfterClamp true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(
                   desc, fuse::renderer::ProbeGridCoord{1u, 0u, 1u}, cache.data(), 8u),
               "in-range coord does not skip cache lookup");
                   desc, fuse::renderer::ProbeGridCoord{99u, 99u, 99u}, cache.data(), 8u),
               "OOB coord that clamps does not skip cache lookup");

    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndexAfterClamp(desc, cache.data(), 999u, 8u, cacheReason),
               "tryValidateCacheIndexAfterClamp succeeds for clampable OOB index");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "clampable OOB index reports no cache reject reason");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(
                   desc, built, cache.data(), 8u),
               "wouldSkip false for valid trilinear sample at coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "wouldSkip false for valid trilinear sample at world position");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(
                   desc, built, nullptr, 8u),
               "wouldSkip true for null cache trilinear sample");

    fuse::renderer::ProbeSampleCoords preflightCoords{};
    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, &preflightCoords, &trilinearReason),
               "preflightTrilinearProbeSample succeeds for interior sample");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "successful trilinear preflight reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::tryPreflightTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, preflightCoords, trilinearReason),
               "tryPreflightTrilinearProbeSample succeeds for interior sample");
               "tryPreflightTrilinearProbeSample reports no reject reason");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "none trilinear reject reason is not blocking");

    fuse::renderer::CacheIndexRejectReason cachePreflightReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightCacheIndexLookup(
                   desc, cache.data(), 3u, 8u, cachePreflightReason),
               "tryPreflightCacheIndexLookup succeeds for valid cache");
    expectTrue(cachePreflightReason == fuse::renderer::CacheIndexRejectReason::None,
               "tryPreflightCacheIndexLookup reports no reject reason");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid rate");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightProbeScheduleAtRate(
                   2048u, 0u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeScheduleAtRate rejects zero probes_per_frame");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "tryPreflightProbeScheduleAtRate reports zero_probes_per_frame reason");

    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
    expectTrue(fuse::renderer::tryPreflightDdgiProbeUpdate(desc, validIndices, 2u, launchReason),
               "tryPreflightDdgiProbeUpdate succeeds for valid launch");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "successful tryPreflightDdgiProbeUpdate reports no reject reason");

void testDdgiPreflightDeepenPass2() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for deepen pass2 trilinear preflight");

        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
    const fuse::math::Vec3 worldInterior{0.5f, 0.5f, 0.5f};
                   desc, worldInterior, cache.data(), 8u),
               "world-position trilinear preflight succeeds for interior sample");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, worldInterior, nullptr, 8u, &trilinearReason),
               "world-position trilinear preflight rejects null cache");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache reports null_cache trilinear reason");

    fuse::renderer::DDGIDesc zeroSpacing = desc;
    zeroSpacing.probe_spacing = {0.f, 2.f, 2.f};
    fuse::renderer::ProbeSampleCoordsRejectReason buildReason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    fuse::renderer::ProbeSampleCoords badBuild{};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   zeroSpacing, {0.5f, 0.5f, 0.5f}, badBuild, buildReason),
               "tryBuildProbeSampleCoords rejects non-sampleable grid");
    expectTrue(buildReason == fuse::renderer::ProbeSampleCoordsRejectReason::NotSampleable,
               "non-sampleable grid reports not_sampleable build reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(buildReason), "not_sampleable") == 0,
               "not_sampleable sample-coord reject reason label");
    const fuse::math::Vec3 interiorSample{0.5f, 0.5f, 0.5f};
                   zeroSpacing, interiorSample, cache.data(), 8u, &trilinearReason),
               "trilinear preflight rejects non-sampleable grid");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::NotSampleable,
               "non-sampleable grid reports not_sampleable trilinear reason");

    fuse::u32 scheduled[2] = {0u, 7u};
    expectTrue(fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(desc, scheduled, 2u, 8u, cacheReason),
               "tryValidateScheduledCacheIndices succeeds for in-range indices");
    expectTrue(fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(
                   desc, cache.data(), scheduled, 2u, 8u, cacheReason),
               "cache-pointer scheduled validation succeeds");
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "preflightCacheIndexLookup succeeds without cache pointer");
    fuse::u32 oobScheduled[2] = {0u, 99u};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(
                   desc, oobScheduled, 2u, 8u, cacheReason),
               "scheduled cache validation rejects OOB index");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB scheduled index reports out_of_range_probe_index");

    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(2048u, 64u, 64u) == 64u,
               "effectiveScheduledProbeCount returns batch size when uncapped");
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(2048u, 64u, 32u) == 32u,
               "effectiveScheduledProbeCount capped by max_indices");
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(0u, 64u, 64u) == 0u,
               "effectiveScheduledProbeCount zero on empty probe count");

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryPreflightProbeKernelLaunch(kernelParams, kernelReason),
               "tryPreflightProbeKernelLaunch succeeds for valid params");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful tryPreflightProbeKernelLaunch reports no reject reason");

void testDdgiThirdPassDeepenGuards() {
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(kernelParams, desc) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "grid-aware classifyProbeKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunch(kernelParams, desc),
               "grid-aware preflightProbeKernelLaunch succeeds for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunch(kernelParams, desc),
               "grid-aware wouldSkipProbeKernelLaunch false for valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeTraceKernel(kernelParams, desc),
               "grid-aware canLaunchProbeTraceKernel true for valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeBlendKernel(kernelParams, desc),
               "grid-aware canLaunchProbeBlendKernel true for valid params");

    fuse::u32 oobKernelIndices[2] = {0u, 99u};
    fuse::renderer::gi::DDGIKernelParams oobParams = kernelParams;
    oobParams.probe_indices_to_update = oobKernelIndices;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(oobParams, desc, kernelReason),
               "grid-aware trace preflight rejects OOB probe index");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex,
               "grid-aware kernel preflight reports out_of_range_probe_index");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(kernelReason),
                           "out_of_range_probe_index") == 0,
               "out_of_range_probe_index kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunch(oobParams, desc),
               "grid-aware wouldSkipProbeKernelLaunch true for OOB index");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(kernelParams, empty) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "grid-aware classifyProbeKernelReject empty_grid");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
}

void testDdgiDeepenGuardPass() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    fuse::renderer::ProbeScheduleRejectReason scheduleReason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");

    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "index-only preflightCacheIndexLookup succeeds for valid index");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "index-only classifyCacheIndexReject out_of_range_probe_index");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "index-only preflightCacheIndexLookup rejects OOB index");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen guard pass");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid coords");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::ProbeSampleCoords preflightCoords{};
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, &preflightCoords),
               "preflightTrilinearProbeIrradiance succeeds for interior sample");
    expectTrue(preflightCoords.x0 == 0u && preflightCoords.x1 == 1u,
               "preflightTrilinearProbeIrradiance returns built coords");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 4u),
               "preflightTrilinearProbeSample rejects undersized cache");

    fuse::renderer::ProbeSampleCoords invalid = built;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(empty, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "classifyProbeTrilinearSampleReject empty grid");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u),
               "preflightTrilinearProbeIrradiance rejects empty grid");

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeTraceKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeBlendKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernelLaunch(kernelParams),
               "preflightProbeTraceKernelLaunch succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernelLaunch(kernelParams),
               "preflightProbeBlendKernelLaunch succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeTraceKernelReject zero_rays_per_probe");
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernelLaunch(zeroRays),
               "preflightProbeTraceKernelLaunch rejects zero rays");

    fuse::renderer::gi::DDGIKernelParams nullIndices = kernelParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(nullIndices) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "classifyProbeBlendKernelReject null_probe_indices");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernelLaunch(nullIndices),
               "preflightProbeBlendKernelLaunch rejects null indices");

    fuse::renderer::DDGIDesc notSampleable = desc;
    notSampleable.probe_spacing = {0.f, 2.f, 2.f};
    fuse::renderer::ProbeSampleCoordsRejectReason sampleReason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::probeSampleCoordsRejectReasonIsBlocking(
                   fuse::renderer::ProbeSampleCoordsRejectReason::NotSampleableGrid),
               "not_sampleable_grid is blocking for sample-coord preflight");
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   notSampleable, {0.5f, 0.5f, 0.5f}, built, sampleReason),
               "zero spacing build fails with not_sampleable_grid");
}

void testDdgiTrilinearSamplePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid sample");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, &reason),
               "preflightTrilinearProbeIrradiance succeeds for interior sample");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "preflightTrilinearProbeIrradiance reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, nullptr, 8u, &reason),
               "preflightProbeTrilinearSample rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache reports null_cache trilinear reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

    expectTrue(!fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 4u, &reason),
               "preflightProbeTrilinearSample rejects undersized cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache trilinear reason");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, invalid, cache.data(), 8u, &reason),
               "preflightProbeTrilinearSample rejects unordered corners");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "unordered corners report invalid_sample_coords trilinear reason");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u, &reason),
               "preflightTrilinearProbeIrradiance rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "empty grid reports empty_grid trilinear reason");
}

void testDdgiScheduleAtRatePreflightGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "successful rate-aware schedule reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count, &reason),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame reports zero_probes_per_frame reason");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 0u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count on rate-aware schedule");
}

void testDdgiSampleCoordSkipPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for sample-coord skip preflight test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobWeights),
               "wouldSkipProbeSampleCoordPreflight false for clampable weights");
}

void testDdgiKernelPerPassPreflightGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeTraceKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeBlendKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(validParams),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(validParams),
               "preflightProbeBlendKernel succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroRays = validParams;
    zeroRays.rays_per_probe = 0u;
    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeTraceKernelReject zero_rays_per_probe");
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroRays, &reason),
               "preflightProbeTraceKernel rejects zero rays per probe");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "preflightProbeTraceKernel reports zero_rays_per_probe reason");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(nullIndices) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "classifyProbeBlendKernelReject null_probe_indices");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(nullIndices, &reason),
               "preflightProbeBlendKernel rejects null probe indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "preflightProbeBlendKernel reports null_probe_indices reason");
}

void testTrilinearSampleDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};
    }

    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearProbeSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSampleAtCoords(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSampleAtCoords succeeds for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(desc, coords, cache.data(), 8u),
               "wouldSkip false for valid coord-based trilinear sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeIrradianceReject(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearProbeIrradianceReject none for interior sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "preflightTrilinearProbeIrradiance succeeds for interior sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "wouldSkip false for valid world-position trilinear sample");

    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyTrilinearProbeSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(desc, coords, nullptr, 8u),
               "wouldSkip true for null cache coord-based sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u),
               "wouldSkip true for null cache world-position sample");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyTrilinearProbeSampleReject invalid_sample_coords");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeIrradianceReject(
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "classifyTrilinearProbeIrradianceReject empty_grid");
}

void testProbeScheduleAtRateDeepenGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "wouldSkip false for valid rate-aware schedule");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testProbeSampleCoordWouldSkip() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldSkip sample-coord test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, oobIndices),
               "wouldSkip true for hard OOB sample coord indices");

    fuse::renderer::DDGIDesc zeroSpacing = desc;
    zeroSpacing.probe_spacing = {0.f, 2.f, 2.f};
    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   zeroSpacing, {0.5f, 0.5f, 0.5f}, built, reason),
               "non-sampleable grid fails tryBuildProbeSampleCoords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::NotSampleableGrid,
               "zero spacing reports not_sampleable_grid reason");
    expectTrue(fuse::renderer::probeSampleCoordsRejectReasonIsBlocking(reason),
               "not_sampleable_grid is blocking for preflight");
}

void testKernelPerPassPreflight() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::preflightProbeTraceKernelLaunch(validParams),
               "preflightProbeTraceKernelLaunch succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernelLaunch(validParams),
               "preflightProbeBlendKernelLaunch succeeds for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip false for valid trace kernel params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip false for valid blend kernel params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernelLaunch(zeroCount, &reason),
               "preflightProbeTraceKernelLaunch rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "trace preflight zero count reports zero_update_count reason");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernelLaunch(zeroCount, &reason),
               "preflightProbeBlendKernelLaunch rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "blend preflight zero count reports zero_update_count reason");
}

void testTrilinearAndSchedulePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "wouldSkipTrilinearProbeIrradiance false for valid world position");

    const fuse::math::Vec3 worldCentre{0.5f, 0.5f, 0.5f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   desc, worldCentre, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid world position");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u),
               "wouldSkipTrilinearProbeIrradiance true for undersized cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords for unordered corners");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    const fuse::math::Vec3 worldOrigin{};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   empty, worldOrigin, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "classifyProbeTrilinearSampleReject empty_grid for empty grid");

    expectTrue(fuse::renderer::probeSampleCoordsRejectReasonIsBlocking(
                   fuse::renderer::ProbeSampleCoordsRejectReason::NotSampleableGrid),
               "not_sampleable_grid sample-coord reject reason is blocking");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid rate");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "successful rate-aware schedule reports no reject reason");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "tryScheduleProbeUpdatesAtRate zero rate reports zero_probes_per_frame reason");
}

void testProbeGridPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeGridReject(desc) ==
                   fuse::renderer::ProbeGridRejectReason::None,
               "classifyProbeGridReject none for sampleable grid");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeGrid(desc),
               "preflightProbeGrid succeeds for sampleable grid");
    expectTrue(!fuse::renderer::probeGridRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridRejectReason::None),
               "none probe-grid reject reason is not blocking");
    expectTrue(fuse::renderer::probeGridRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridRejectReason::EmptyGrid),
               "empty_grid probe-grid reject reason is blocking");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeGridAccess(desc),
               "wouldSkipProbeGridAccess false for sampleable grid");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 3u) ==
                   fuse::renderer::ProbeGridRejectReason::None,
               "classifyProbeIndexReject none for in-range index");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeIndex(desc, 3u),
               "preflightProbeIndex succeeds for in-range index");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 99u) ==
                   fuse::renderer::ProbeGridRejectReason::OutOfRangeProbeIndex,
               "classifyProbeIndexReject out_of_range_probe_index");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(
                               fuse::renderer::ProbeGridRejectReason::OutOfRangeProbeIndex),
                           "out_of_range_probe_index") == 0,
               "out_of_range_probe_index probe-grid reject reason label");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, validCoord) ==
                   fuse::renderer::ProbeGridRejectReason::None,
               "classifyProbeCoordReject none for valid coord");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeCoord(desc, validCoord),
               "preflightProbeCoord succeeds for valid coord");

    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, invalidCoord) ==
                   fuse::renderer::ProbeGridRejectReason::OutOfRangeProbeCoord,
               "classifyProbeCoordReject out_of_range_probe_coord");
    expectTrue(!fuse::renderer::ProbeGridLayout::preflightProbeCoord(desc, invalidCoord),
               "preflightProbeCoord rejects OOB coord");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeGridReject(empty) ==
                   fuse::renderer::ProbeGridRejectReason::EmptyGrid,
               "classifyProbeGridReject empty_grid");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeGridAccess(empty),
               "wouldSkipProbeGridAccess true for empty grid");

    fuse::renderer::DDGIDesc notSampleable = desc;
    notSampleable.irradiance_res = 0u;
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeGridReject(notSampleable) ==
                   fuse::renderer::ProbeGridRejectReason::NotSampleable,
               "classifyProbeGridReject not_sampleable");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(
                               fuse::renderer::ProbeGridRejectReason::NotSampleable),
                           "not_sampleable") == 0,
               "not_sampleable probe-grid reject reason label");
}

void testProbeTrilinearPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   desc, coords, cache.data(), 8u) == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    const fuse::math::Vec3 centre{0.5f, 0.5f, 0.5f};
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, centre, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for interior sample");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "coord-based preflightProbeTrilinearSample succeeds");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, centre, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   desc, centre, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, centre, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   desc, centre, cache.data(), 4u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::UndersizedCache,
               "classifyProbeTrilinearSampleReject undersized cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid sample coords");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   empty, fuse::math::Vec3{}, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "classifyProbeTrilinearSampleReject empty grid");
}

void testProbeScheduleAtRatePreflightGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(
                   2048u, 64u, 64u, indices, &count) == fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(
                   2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testProbeKernelPreflightGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeTraceKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeBlendKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(validParams),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(validParams),
               "preflightProbeBlendKernel succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(nullIndices) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "classifyProbeBlendKernelReject null_probe_indices");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(nullIndices),
               "preflightProbeBlendKernel rejects null indices");
}

void testDdgiTrilinearPreflightDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, nullptr, 8u),
               "preflightTrilinearProbeSample rejects null cache");

    fuse::renderer::ProbeSampleCoords invalid = built;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for unordered corners");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), invalidCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, nullptr, validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for null cache");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testDdgiTrilinearDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear deepen test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, coords),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = coords;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};
    }

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::canTrilinearSampleAtProbeCoords(desc, coords, cache.data(), 8u),
               "canTrilinearSampleAtProbeCoords true for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::tryTrilinearSampleAtProbeCoords(
                   desc, coords, cache.data(), 8u, reason),
               "tryTrilinearSampleAtProbeCoords succeeds for valid inputs");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "valid trilinear preflight reports no reject reason");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(reason),
               "none trilinear reject reason is not blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid inputs");

    fuse::renderer::ProbeSampleCoords oobWeights = coords;
    oobWeights.tx = 2.f;
    expectTrue(fuse::renderer::ddgi_util::tryTrilinearSampleAtProbeCoords(
                   desc, oobWeights, cache.data(), 8u, reason),
               "tryTrilinearSampleAtProbeCoords soft-passes clampable weights");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::ClampableWeights,
               "clampable weights report clampable_weights reason");
    expectTrue(std::strcmp(fuse::renderer::probeTrilinearSampleRejectReasonLabel(reason), "clampable_weights") == 0,
               "clampable_weights trilinear reject reason label");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(reason),
               "clampable_weights is not blocking for trilinear preflight");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, oobWeights, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for clampable weights");

    expectTrue(!fuse::renderer::ddgi_util::tryTrilinearSampleAtProbeCoords(
                   desc, coords, nullptr, 8u, reason),
               "tryTrilinearSampleAtProbeCoords rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache reports null_cache trilinear reason");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(reason),
               "null_cache is blocking for trilinear preflight");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

    fuse::math::Vec3 sampled{};
    expectTrue(fuse::renderer::ddgi_util::tryTrilinearProbeIrradianceAtCoords(
                   desc, coords, cache.data(), 8u, sampled, reason),
               "tryTrilinearProbeIrradianceAtCoords succeeds on full cache");
    expectTrue(sampled.x > 0.f, "coords-based trilinear sample is non-zero");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "coords-based trilinear sample reports no reject reason");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(
                   desc, validCoord, cache.data(), 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(
                   desc, invalidCoord, cache.data(), 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(
                   2048u, 64u, 64u, indices, &count) == fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(
                   2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testDdgiTrilinearAndWouldSkipGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};
    }

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear wouldSkip test");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeLookup(desc, cache.data(), 8u),
               "accessible cache does not skip probe lookup");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeLookup(desc, nullptr, 8u),
               "null cache skips probe lookup");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeLookup(desc, cache.data(), 4u),
               "undersized cache skips probe lookup");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), {0, 0, 0}, 8u),
               "in-range coord does not skip cache lookup");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), {9, 9, 9}, 8u),
               "hard OOB coord skips cache lookup");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, nullptr, {0, 0, 0}, 8u),
               "null cache skips coord cache lookup");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "valid trilinear sample does not skip");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, nullptr, 8u),
               "null cache skips trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 4u),
               "undersized cache skips trilinear sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::math::Vec3 sampled{};
    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryTrilinearProbeIrradianceAtCoords(
                   desc, coords, cache.data(), 8u, sampled, trilinearReason),
               "tryTrilinearProbeIrradianceAtCoords succeeds on full cache");
    expectTrue(sampled.x > 0.f, "coord-based trilinear sample is non-zero");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "coord-based trilinear sample reports no reject reason");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, coords),
               "valid sample coords do not skip preflight");
    fuse::renderer::ProbeSampleCoords oobIndices = coords;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, oobIndices),
               "hard OOB sample coords skip preflight");

    const fuse::renderer::ProbeGridCoord validCoord{0, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeCoord(desc, validCoord),
               "preflightProbeCoord succeeds for origin coord");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, validCoord) ==
                   fuse::renderer::ProbeGridCoordRejectReason::None,
               "classifyProbeCoordReject none for valid coord");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordPreflight(desc, validCoord),
               "valid coord does not skip probe coord preflight");

    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, invalidCoord) ==
                   fuse::renderer::ProbeGridCoordRejectReason::OutOfRangeCoord,
               "classifyProbeCoordReject out_of_range_coord");
    expectTrue(std::strcmp(fuse::renderer::probeGridCoordRejectReasonLabel(
                               fuse::renderer::ProbeGridCoordRejectReason::OutOfRangeCoord),
                           "out_of_range_coord") == 0,
               "out_of_range_coord probe-grid reject reason label");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordPreflight(desc, invalidCoord),
               "OOB coord skips probe coord preflight");
    expectTrue(fuse::renderer::probeGridCoordRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridCoordRejectReason::OutOfRangeCoord),
               "out_of_range_coord is blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(empty, validCoord) ==
                   fuse::renderer::ProbeGridCoordRejectReason::EmptyGrid,
               "empty grid classifyProbeCoordReject reports empty_grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeLookup(empty, cache.data(), 8u),
               "empty grid skips probe lookup");
}

void testDdgiTrilinearPreflightDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    const fuse::math::Vec3 interiorPos{0.5f, 0.5f, 0.5f};
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, interiorPos, cache.data(), 8u),
               "preflightTrilinearProbeSample world_position succeeds for interior sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, nullptr, 8u),
               "preflightTrilinearProbeSample rejects null cache");

    fuse::renderer::ProbeSampleCoords invalid = built;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for unordered corners");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), invalidCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, nullptr, validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for null cache");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(2048u, 64u, 32u) == 32u,
               "effectiveScheduledProbeCount caps at max_indices");
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(2048u, 64u, 64u) == 64u,
               "effectiveScheduledProbeCount matches probes_per_frame when capacity allows");

    fuse::u32 scheduledIndices[4] = {0u, 3u, 7u, 99u};
    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(
                   desc, scheduledIndices, 3u, 8u, cacheReason),
               "tryValidateScheduledCacheIndices passes for in-range indices");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(
                   desc, scheduledIndices, 4u, 8u, cacheReason),
               "tryValidateScheduledCacheIndices rejects OOB index in batch");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "scheduled cache validation reports out_of_range_probe_index");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(
                   desc, cache.data(), scheduledIndices, 4u, 8u, cacheReason),
               "cache-pointer scheduled validation rejects OOB index");
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "preflightCacheIndexLookup without cache pointer succeeds");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "preflightCacheIndexLookup without cache pointer rejects OOB index");
}

void testDdgiKernelGridAwarePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = validIndices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(validParams, desc) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelReject none for valid grid-aware params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunch(validParams, desc),
               "preflightProbeKernelLaunch grid-aware succeeds for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunch(validParams, desc),
               "wouldSkipProbeKernelLaunch grid-aware false for valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeTraceKernel(validParams, desc),
               "canLaunchProbeTraceKernel grid-aware true for valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeBlendKernel(validParams, desc),
               "canLaunchProbeBlendKernel grid-aware true for valid params");

    fuse::u32 oobIndices[2] = {0u, 99u};
    fuse::renderer::gi::DDGIKernelParams oobParams = validParams;
    oobParams.probe_indices_to_update = oobIndices;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(oobParams, desc) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex,
               "classifyProbeKernelReject out_of_range_probe_index for OOB index");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunch(oobParams, desc),
               "wouldSkipProbeKernelLaunch grid-aware true for OOB index");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                   fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex),
               "out_of_range_probe_index") == 0,
               "out_of_range_probe_index kernel reject reason label");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(validParams, empty) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelReject empty_grid for empty volume");
    expectTrue(!fuse::renderer::gi::preflightProbeKernelLaunch(validParams, empty),
               "preflightProbeKernelLaunch grid-aware rejects empty grid");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
               "empty_grid") == 0,
               "empty_grid kernel reject reason label");
}

void testProbeGridPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 3u) ==
                   fuse::renderer::ProbeGridRejectReason::None,
               "classifyProbeIndexReject none for valid index");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeIndex(desc, 3u),
               "preflightProbeIndex succeeds for valid index");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeIndex(desc, 3u),
               "wouldSkipProbeIndex false for valid index");
    expectTrue(!fuse::renderer::probeGridRejectReasonIsBlocking(fuse::renderer::ProbeGridRejectReason::None),
               "none probe-grid reject reason is not blocking");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 99u) ==
                   fuse::renderer::ProbeGridRejectReason::OutOfRangeIndex,
               "classifyProbeIndexReject out_of_range_index");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeIndex(desc, 99u),
               "wouldSkipProbeIndex true for OOB index");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(
                               fuse::renderer::ProbeGridRejectReason::OutOfRangeIndex),
                           "out_of_range_index") == 0,
               "out_of_range_index probe-grid reject reason label");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, validCoord) ==
                   fuse::renderer::ProbeGridRejectReason::None,
               "classifyProbeCoordReject none for valid coord");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeCoord(desc, validCoord),
               "preflightProbeCoord succeeds for valid coord");

    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, invalidCoord) ==
                   fuse::renderer::ProbeGridRejectReason::OutOfRangeCoord,
               "classifyProbeCoordReject out_of_range_coord");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeCoord(desc, invalidCoord),
               "wouldSkipProbeCoord true for OOB coord");
    expectTrue(fuse::renderer::probeGridRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridRejectReason::OutOfRangeCoord),
               "out_of_range_coord probe-grid reject reason is blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(empty, 0u) ==
                   fuse::renderer::ProbeGridRejectReason::EmptyGrid,
               "classifyProbeIndexReject empty_grid");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(
                               fuse::renderer::ProbeGridRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid probe-grid reject reason label");

    fuse::renderer::ProbeGridRejectReason gridReason = fuse::renderer::ProbeGridRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryValidateProbeIndex(desc, 7u, gridReason),
               "tryValidateProbeIndex succeeds for last index");
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeCoord(empty, validCoord, gridReason),
               "tryValidateProbeCoord rejects empty grid");
    expectTrue(gridReason == fuse::renderer::ProbeGridRejectReason::EmptyGrid,
               "empty grid coord validation reports empty_grid reason");
}

void testTrilinearSamplePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid coords");
    const fuse::math::Vec3 worldCentre{0.5f, 0.5f, 0.5f};
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, worldCentre, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for world position");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 4u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::UndersizedCache,
               "classifyProbeTrilinearSampleReject undersized cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid sample coords");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    const fuse::math::Vec3 worldOrigin{0.f, 0.f, 0.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   empty, worldOrigin, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "classifyProbeTrilinearSampleReject empty grid from world position");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
                   empty, worldOrigin, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for empty grid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   zeroRes, worldCentre, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NotSampleable,
               "classifyProbeTrilinearSampleReject not_sampleable from world position");
}

void testProbeGridRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    const fuse::renderer::ProbeGridCoord valid{1u, 0u, 1u};
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeCoord(desc, valid),
               "preflightProbeCoord succeeds for valid coord");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, valid) ==
                   fuse::renderer::ProbeGridRejectReason::None,
               "classifyProbeCoordReject none for valid coord");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordLookup(desc, valid),
               "wouldSkipProbeCoordLookup false for valid coord");

    const fuse::renderer::ProbeGridCoord invalid{9u, 0u, 0u};
    fuse::renderer::ProbeGridRejectReason gridReason = fuse::renderer::ProbeGridRejectReason::None;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeCoord(desc, invalid, gridReason),
               "tryValidateProbeCoord rejects OOB coord");
    expectTrue(gridReason == fuse::renderer::ProbeGridRejectReason::InvalidCoord,
               "OOB coord reports invalid_coord reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(gridReason), "invalid_coord") == 0,
               "invalid_coord probe-grid reject reason label");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordLookup(desc, invalid),
               "wouldSkipProbeCoordLookup true for invalid coord");

    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeIndex(desc, 3u),
               "preflightProbeIndex succeeds for in-range index");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 3u) ==
                   fuse::renderer::ProbeGridRejectReason::None,
               "classifyProbeIndexReject none for valid index");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeIndexLookup(desc, 3u),
               "wouldSkipProbeIndexLookup false for valid index");

    expectTrue(!fuse::renderer::ProbeGridLayout::preflightProbeIndex(desc, 99u),
               "preflightProbeIndex rejects OOB index");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 99u) ==
                   fuse::renderer::ProbeGridRejectReason::OutOfRangeIndex,
               "classifyProbeIndexReject out_of_range_index");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeIndexLookup(desc, 99u),
               "wouldSkipProbeIndexLookup true for OOB index");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(empty, 0u) ==
                   fuse::renderer::ProbeGridRejectReason::EmptyGrid,
               "classifyProbeIndexReject empty_grid on empty volume");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(
                   fuse::renderer::ProbeGridRejectReason::EmptyGrid),
               "empty_grid") == 0,
               "empty_grid probe-grid reject reason label");
    expectTrue(!fuse::renderer::probeGridRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridRejectReason::None),
               "none probe-grid reject reason is not blocking");
    expectTrue(fuse::renderer::probeGridRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridRejectReason::InvalidCoord),
               "invalid_coord probe-grid reject reason is blocking");
}

void testTrilinearSamplePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null_cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 4u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::UndersizedCache,
               "classifyProbeTrilinearSampleReject undersized_cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 4u),
               "preflightProbeTrilinearSample rejects undersized cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;
    desc.depth_res = 16u;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init, reason),
               "valid desc passes init source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid init source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "preflightProbeGridSource succeeds for valid init desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "wouldSkip false for valid init desc");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   empty, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid for init");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   empty, fuse::renderer::ProbeGridSourceKind::Sample, reason),
               "empty grid fails sample source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid sample source reports empty_grid reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroIrradiance, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res for init");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroDepth, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res for init");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   badSpacing, fuse::renderer::ProbeGridSourceKind::Sample) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing for sample");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRays, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe for update");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe),
                           "zero_rays_per_probe") == 0,
               "zero_rays_per_probe probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRate = desc;
    zeroRate.probes_per_frame = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRate, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroProbesPerFrame,
               "classifyProbeGridSourceReject zero_probes_per_frame for update");
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Update, reason),
               "valid desc passes update source validation");
}

void testTrilinearSamplePreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, nullptr, 8u, &reason),
               "preflightTrilinearProbeSample rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache trilinear preflight reports null_cache reason");
}

void testScheduleAtRateDeepen() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule-at-rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelDescPreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectForDesc none for valid desc+params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchForDesc(desc, kernelParams),
               "preflightProbeKernelLaunchForDesc succeeds for valid desc+params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc false for valid desc+params");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(empty, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelRejectForDesc empty_grid for empty desc");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(empty, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc true for empty desc");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelRejectForDesc zero_rays_per_probe after empty-grid check");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;
    desc.depth_res = 16u;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init, reason),
               "valid desc passes init source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid init source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "preflightProbeGridSource succeeds for valid init desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "wouldSkip false for valid init desc");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   empty, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid for init");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   empty, fuse::renderer::ProbeGridSourceKind::Sample, reason),
               "empty grid fails sample source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid sample source reports empty_grid reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroIrradiance, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res for init");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroDepth, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res for init");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   badSpacing, fuse::renderer::ProbeGridSourceKind::Sample) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing for sample");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRays, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe for update");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe),
                           "zero_rays_per_probe") == 0,
               "zero_rays_per_probe probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRate = desc;
    zeroRate.probes_per_frame = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRate, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroProbesPerFrame,
               "classifyProbeGridSourceReject zero_probes_per_frame for update");
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Update, reason),
               "valid desc passes update source validation");
}

void testTrilinearSamplePreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, nullptr, 8u, &reason),
               "preflightTrilinearProbeSample rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache trilinear preflight reports null_cache reason");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");

    fuse::renderer::ProbeSampleCoords oobWeights = coords;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, coords),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobWeights),
               "wouldSkipProbeSampleCoordPreflight false for clampable weights");

    fuse::renderer::ProbeSampleCoords oobIndices = coords;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord validCoord{1u, 0u, 1u};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(
                   desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9u, 0u, 0u};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(
                   desc, cache.data(), invalidCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(
                   desc, nullptr, validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for null cache");
}

void testScheduleAtRateDeepen() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule-at-rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelDescPreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectForDesc none for valid desc+params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchForDesc(desc, kernelParams),
               "preflightProbeKernelLaunchForDesc succeeds for valid desc+params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc false for valid desc+params");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(empty, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelRejectForDesc empty_grid for empty desc");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(empty, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc true for empty desc");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelRejectForDesc zero_rays_per_probe after empty-grid check");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeIndexRejectReason indexReason = fuse::renderer::ProbeIndexRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryValidateProbeIndex(desc, 3u, indexReason),
               "in-range probe index passes source validation");
    expectTrue(indexReason == fuse::renderer::ProbeIndexRejectReason::None,
               "valid probe index reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeIndexRejectReasonLabel(indexReason), "none") == 0,
               "none probe-index reject reason label");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeIndex(desc, 3u),
               "preflightProbeIndex succeeds for in-range index");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeIndexLookup(desc, 3u),
               "wouldSkipProbeIndexLookup false for valid index");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 3u) ==
                   fuse::renderer::ProbeIndexRejectReason::None,
               "classifyProbeIndexReject none for valid index");
    expectTrue(!fuse::renderer::probeIndexRejectReasonIsBlocking(indexReason),
               "none probe-index reject reason is not blocking");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeIndex(desc, 99u, indexReason),
               "OOB probe index fails source validation");
    expectTrue(indexReason == fuse::renderer::ProbeIndexRejectReason::OutOfRangeIndex,
               "OOB probe index reports out_of_range_index reason");
    expectTrue(std::strcmp(fuse::renderer::probeIndexRejectReasonLabel(indexReason), "out_of_range_index") == 0,
               "out_of_range_index probe-index reject reason label");
    expectTrue(fuse::renderer::probeIndexRejectReasonIsBlocking(indexReason),
               "out_of_range_index probe-index reject reason is blocking");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeIndexLookup(desc, 99u),
               "wouldSkipProbeIndexLookup true for OOB index");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    fuse::renderer::ProbeCoordRejectReason coordReason = fuse::renderer::ProbeCoordRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryValidateProbeCoord(desc, validCoord, coordReason),
               "in-range probe coord passes source validation");
    expectTrue(coordReason == fuse::renderer::ProbeCoordRejectReason::None,
               "valid probe coord reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeCoordRejectReasonLabel(coordReason), "none") == 0,
               "none probe-coord reject reason label");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeCoord(desc, validCoord),
               "preflightProbeCoord succeeds for in-range coord");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordLookup(desc, validCoord),
               "wouldSkipProbeCoordLookup false for valid coord");

    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeCoord(desc, invalidCoord, coordReason),
               "OOB probe coord fails source validation");
    expectTrue(coordReason == fuse::renderer::ProbeCoordRejectReason::OutOfRangeCoord,
               "OOB probe coord reports out_of_range_coord reason");
    expectTrue(std::strcmp(fuse::renderer::probeCoordRejectReasonLabel(coordReason), "out_of_range_coord") == 0,
               "out_of_range_coord probe-coord reject reason label");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, invalidCoord) ==
                   fuse::renderer::ProbeCoordRejectReason::OutOfRangeCoord,
               "classifyProbeCoordReject out_of_range_coord");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeIndex(empty, 0u, indexReason),
               "empty grid fails probe-index source validation");
    expectTrue(indexReason == fuse::renderer::ProbeIndexRejectReason::EmptyGrid,
               "empty grid probe index reports empty_grid reason");
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeCoord(empty, validCoord, coordReason),
               "empty grid fails probe-coord source validation");
    expectTrue(coordReason == fuse::renderer::ProbeCoordRejectReason::EmptyGrid,
               "empty grid probe coord reports empty_grid reason");
}

void testTrilinearSamplePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds on accessible grid");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(reason),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, nullptr, 8u, &reason),
               "preflightTrilinearProbeSample rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache trilinear preflight reports null_cache reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, invalid, cache.data(), 8u, &reason),
               "preflightTrilinearProbeSample rejects unordered corners");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "unordered corners trilinear preflight reports invalid_sample_coords reason");
}

void testCacheIndexPreflightOverload() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "preflightCacheIndexLookup succeeds without cache pointer");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "preflightCacheIndexLookup rejects OOB index without cache pointer");

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u, &reason),
               "preflightCacheIndexLookup with reason succeeds without cache pointer");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache-index preflight without pointer reports no reject reason");
}

void testScheduleAtRatePreflightGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");

    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "tryScheduleProbeUpdatesAtRate zero rate reports zero_probes_per_frame reason");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelPreflightDeepenGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(validParams),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(validParams),
               "preflightProbeBlendKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeTraceKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeBlendKernelReject none for valid params");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(nullIndices),
               "preflightProbeBlendKernel rejects null probe indices");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(nullIndices) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "classifyProbeBlendKernelReject null_probe_indices");

    fuse::renderer::gi::DDGIKernelParams zeroRays = validParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroRays),
               "preflightProbeTraceKernel rejects zero rays per probe");
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeTraceKernelReject zero_rays_per_probe");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;
    desc.depth_res = 16u;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init, reason),
               "valid desc passes init source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid init source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "preflightProbeGridSource succeeds for valid init desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "wouldSkip false for valid init desc");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   empty, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid for init");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   empty, fuse::renderer::ProbeGridSourceKind::Sample, reason),
               "empty grid fails sample source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid sample source reports empty_grid reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroIrradiance, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res for init");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroDepth, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res for init");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   badSpacing, fuse::renderer::ProbeGridSourceKind::Sample) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing for sample");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRays, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe for update");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe),
                           "zero_rays_per_probe") == 0,
               "zero_rays_per_probe probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRate = desc;
    zeroRate.probes_per_frame = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRate, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroProbesPerFrame,
               "classifyProbeGridSourceReject zero_probes_per_frame for update");
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Update, reason),
               "valid desc passes update source validation");
}

void testTrilinearSamplePreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, nullptr, 8u, &reason),
               "preflightTrilinearProbeSample rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache trilinear preflight reports null_cache reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyTrilinearSampleReject invalid_sample_coords");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for unordered corners");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, coords),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    fuse::renderer::ProbeSampleCoords oobIndices = coords;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), invalidCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, nullptr, validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for null cache");
}

void testScheduleAtRateDeepen() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule-at-rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelDescPreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectForDesc none for valid desc+params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchForDesc(desc, kernelParams),
               "preflightProbeKernelLaunchForDesc succeeds for valid desc+params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc false for valid desc+params");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(empty, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelRejectForDesc empty_grid for empty desc");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(empty, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc true for empty desc");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelRejectForDesc zero_rays_per_probe after empty-grid check");
}

void testProbeGridIndexBoundsHelpers() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(desc) == 7u,
               "maxProbeIndex returns last flat index");
    expectTrue(fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(7u, desc),
               "index 7 is at max for 2x2x2 grid");
    expectTrue(!fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(6u, desc),
               "index 6 is not at max for 2x2x2 grid");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(empty) == 0u,
               "maxProbeIndex returns 0 on empty grid");
    expectTrue(!fuse::renderer::ProbeGridLayout::isAtMaxProbeIndex(0u, empty),
               "isAtMaxProbeIndex false on empty grid");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    const fuse::renderer::ProbeGridSource validSource{&desc, cache.data(), 8u};

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(validSource, reason),
               "accessible probe-grid source passes validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(validSource),
               "preflightProbeGridSource succeeds for accessible source");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(validSource),
               "wouldSkip false for accessible source");

    const fuse::renderer::ProbeGridSource nullDesc{};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(nullDesc, reason),
               "null desc fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::NullDesc,
               "null desc reports null_desc reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "null_desc") == 0,
               "null_desc probe-grid source reject reason label");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "null_desc reject reason is blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    const fuse::renderer::ProbeGridSource emptySource{&empty, cache.data(), 8u};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(emptySource, reason),
               "empty grid fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid reports empty_grid source reason");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    const fuse::renderer::ProbeGridSource notSampleable{&zeroRes, cache.data(), 8u};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(notSampleable, reason),
               "non-sampleable grid fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "zero irradiance_res reports not_sampleable source reason");

    const fuse::renderer::ProbeGridSource nullCache{&desc, nullptr, 8u};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(nullCache, reason),
               "null cache fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::NullCache,
               "null cache reports null_cache source reason");

    const fuse::renderer::ProbeGridSource undersized{&desc, cache.data(), 4u};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(undersized, reason),
               "undersized cache fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache source reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(undersized),
               "wouldSkip true for undersized source");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(undersized) ==
                   fuse::renderer::ProbeGridSourceRejectReason::UndersizedCache,
               "classifyProbeGridSourceReject undersized_cache");
}

void testPreflightTrilinearProbeSample() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;

    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, &reason),
               "preflightTrilinearProbeSample succeeds on full cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "successful trilinear preflight reports no reject reason");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(reason),
               "none trilinear reject reason is not blocking");

    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u, &reason),
               "preflightTrilinearProbeSample rejects undersized cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::UndersizedCache,
               "undersized trilinear preflight reports undersized_cache reason");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(reason),
               "undersized_cache trilinear reject reason is blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u, &reason),
               "preflightTrilinearProbeSample rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "empty grid trilinear preflight reports empty_grid reason");
}

void testKernelOobProbeIndexGuards() {
    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::u32 oobIndices[2] = {0u, 99u};

    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = validIndices;
    validParams.probe_update_count = 2u;
    validParams.probe_grid_count = 8u;
    validParams.rays_per_probe = 256u;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(validParams, reason),
               "kernel preflight succeeds when indices fit probe_grid_count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "valid kernel params report no reject reason");

    fuse::renderer::gi::DDGIKernelParams oobParams = validParams;
    oobParams.probe_indices_to_update = oobIndices;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(oobParams, reason),
               "kernel preflight rejects OOB probe index when probe_grid_count set");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex,
               "OOB probe index reports out_of_range_probe_index kernel reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "out_of_range_probe_index") ==
                   0,
               "out_of_range_probe_index kernel reject reason label");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(oobParams, nullptr),
               "trace kernel launch rejects OOB indices when probe_grid_count set");

    fuse::renderer::gi::DDGIKernelParams legacyParams = validParams;
    legacyParams.probe_indices_to_update = oobIndices;
    legacyParams.probe_grid_count = 0u;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(legacyParams, reason),
               "kernel preflight skips index range check when probe_grid_count is zero");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(legacyParams, nullptr),
               "trace kernel launch unchanged when probe_grid_count is zero");

    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    fuse::renderer::gi::DDGIKernelParams populated{};
    fuse::renderer::gi::populateDDGIKernelParams(populated, desc, validIndices, 2u, 7u);
    expectTrue(populated.probe_grid_count == 8u, "populateDDGIKernelParams sets probe_grid_count");
    expectTrue(populated.frame_seed == 7u, "populateDDGIKernelParams preserves frame_seed");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;
    desc.depth_res = 16u;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init, reason),
               "valid desc passes init source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid init source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "preflightProbeGridSource succeeds for valid init desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "wouldSkipProbeGridSource false for valid init desc");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   empty, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid for init");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   empty, fuse::renderer::ProbeGridSourceKind::Sample, reason),
               "empty grid fails sample source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid sample source reports empty_grid reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroIrradiance, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res for init");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroDepth, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res for init");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   badSpacing, fuse::renderer::ProbeGridSourceKind::Sample) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing for sample");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRays, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe for update");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe),
                           "zero_rays_per_probe") == 0,
               "zero_rays_per_probe probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRate = desc;
    zeroRate.probes_per_frame = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRate, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroProbesPerFrame,
               "classifyProbeGridSourceReject zero_probes_per_frame for update");
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Update, reason),
               "valid desc passes update source validation");
}

void testDdgiTrilinearPreflightDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, nullptr, 8u),
               "preflightTrilinearProbeSample rejects null cache");

    fuse::renderer::ProbeSampleCoords invalid = built;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for unordered corners");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), invalidCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, nullptr, validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for null cache");
}

void testScheduleAtRateDeepen() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule-at-rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelDescPreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectForDesc none for valid desc+params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchForDesc(desc, kernelParams),
               "preflightProbeKernelLaunchForDesc succeeds for valid desc+params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc false for valid desc+params");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(empty, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelRejectForDesc empty_grid for empty desc");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(empty, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc true for empty desc");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelRejectForDesc zero_rays_per_probe after empty-grid check");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    const fuse::renderer::ProbeGridCoord valid{1, 0, 1};
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordLookup(desc, valid),
               "in-range coord does not skip probe-coord lookup");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeCoordLookup(desc, valid),
               "preflightProbeCoordLookup succeeds for in-range coord");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, valid) ==
                   fuse::renderer::ProbeCoordRejectReason::None,
               "classifyProbeCoordReject none for valid coord");
    expectTrue(!fuse::renderer::probeCoordRejectReasonIsBlocking(
                   fuse::renderer::ProbeCoordRejectReason::None),
               "none probe-coord reject reason is not blocking");
    expectTrue(std::strcmp(fuse::renderer::probeCoordRejectReasonLabel(
                               fuse::renderer::ProbeCoordRejectReason::None),
                           "none") == 0,
               "none probe-coord reject reason label");

    const fuse::renderer::ProbeGridCoord invalid{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordLookup(desc, invalid),
               "OOB coord skips probe-coord lookup");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(desc, invalid) ==
                   fuse::renderer::ProbeCoordRejectReason::OutOfRangeCoord,
               "OOB coord reports out_of_range_coord reason");
    expectTrue(std::strcmp(fuse::renderer::probeCoordRejectReasonLabel(
                               fuse::renderer::ProbeCoordRejectReason::OutOfRangeCoord),
                           "out_of_range_coord") == 0,
               "out_of_range_coord probe-coord reject reason label");
    expectTrue(fuse::renderer::probeCoordRejectReasonIsBlocking(
                   fuse::renderer::ProbeCoordRejectReason::OutOfRangeCoord),
               "out_of_range_coord probe-coord reject reason is blocking");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeIndexLookup(desc, 3u),
               "in-range index does not skip probe-index lookup");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeIndexLookup(desc, 3u),
               "preflightProbeIndexLookup succeeds for in-range index");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 3u) ==
                   fuse::renderer::ProbeIndexRejectReason::None,
               "classifyProbeIndexReject none for valid index");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeIndexLookup(desc, 99u),
               "OOB index skips probe-index lookup");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(desc, 99u) ==
                   fuse::renderer::ProbeIndexRejectReason::OutOfRangeProbeIndex,
               "OOB index reports out_of_range_probe_index reason");
    expectTrue(std::strcmp(fuse::renderer::probeIndexRejectReasonLabel(
                               fuse::renderer::ProbeIndexRejectReason::OutOfRangeProbeIndex),
                           "out_of_range_probe_index") == 0,
               "out_of_range_probe_index probe-index reject reason label");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordLookup(empty, valid),
               "empty grid skips probe-coord lookup");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordReject(empty, valid) ==
                   fuse::renderer::ProbeCoordRejectReason::EmptyGrid,
               "empty grid coord reports empty_grid reason");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeIndexLookup(empty, 0u),
               "empty grid skips probe-index lookup");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexReject(empty, 0u) ==
                   fuse::renderer::ProbeIndexRejectReason::EmptyGrid,
               "empty grid index reports empty_grid reason");
}

void testTrilinearWouldSkipGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear wouldSkip test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "accessible cache does not skip trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds on accessible grid");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "world-position trilinear does not skip on accessible grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "directional trilinear does not skip on accessible grid");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "null cache skips trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache reports null_cache trilinear reason");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 4u),
               "undersized cache skips trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u),
               "undersized cache skips world-position trilinear");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "unordered corners skip trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "unordered corners report invalid_sample_coords trilinear reason");

    fuse::renderer::ProbeSampleCoords oobWeights = coords;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordsPreflight(desc, oobWeights),
               "clampable weights do not skip sample-coord preflight");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordsPreflight(desc, invalid),
               "unordered corners do not skip sample-coord preflight");

    fuse::renderer::ProbeSampleCoords oobIndices = coords;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordsPreflight(desc, oobIndices),
               "hard OOB indices skip sample-coord preflight");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u),
               "empty grid skips world-position trilinear");
}

void testScheduleAtRatePreflightGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "successful trySchedule at rate reports no reject reason");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "trySchedule at rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
}

void testCacheIndexCountOnlyPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "count-only preflightCacheIndexLookup succeeds for valid index");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "count-only preflightCacheIndexLookup rejects OOB index");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 2u),
               "count-only preflightCacheIndexLookup rejects undersized cache");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(empty, 0u, 8u),
               "count-only preflightCacheIndexLookup rejects empty grid");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryValidateProbeIndexSource(desc, 3u, reason),
               "in-range probe index source passes validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid probe index source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");

    expectTrue(fuse::renderer::ProbeGridLayout::tryValidateProbeIndexCoordRoundTrip(desc, 3u, reason),
               "probe index round-trip passes validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "round-trip reports no reject reason");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeIndexSource(desc, 99u, reason),
               "OOB probe index source fails validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::OutOfRangeIndex,
               "OOB probe index reports out_of_range_index reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "out_of_range_index") == 0,
               "out_of_range_index probe-grid source reject reason label");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(fuse::renderer::ProbeGridLayout::tryValidateProbeCoordSource(desc, validCoord, reason),
               "valid probe coord source passes validation");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeGridCoordSourceReject(desc, validCoord) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridCoordSourceReject none for valid coord");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeGridCoordSource(desc, validCoord),
               "preflightProbeGridCoordSource succeeds for valid coord");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeGridCoordSource(desc, validCoord),
               "wouldSkipProbeGridCoordSource false for valid coord");

    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeCoordSource(desc, invalidCoord, reason),
               "OOB probe coord source fails validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::OutOfRangeCoord,
               "OOB probe coord reports out_of_range_coord reason");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeGridIndexSourceReject(desc, 99u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::OutOfRangeIndex,
               "classifyProbeGridIndexSourceReject out_of_range_index");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeGridIndexSource(desc, 3u),
               "preflightProbeGridIndexSource succeeds for valid index");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeGridIndexSource(desc, 99u),
               "wouldSkipProbeGridIndexSource true for OOB index");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::OutOfRangeIndex),
               "out_of_range_index probe-grid source reject reason is blocking");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe-grid source reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryValidateProbeIndexSource(empty, 0u, reason),
               "empty grid probe index source fails validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid probe index source reports empty_grid reason");
}

void testTrilinearAndSchedulePreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "preflightCacheIndexLookup index-only succeeds for valid index");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "classifyCacheIndexReject out_of_range_probe_index for index-only overload");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");

    fuse::renderer::ProbeSampleCoords oobWeights = coords;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobWeights),
               "wouldSkipProbeSampleCoordPreflight false for clampable weights");
    fuse::renderer::ProbeSampleCoords oobIndices = coords;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");
}

void testGridAwareKernelPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.rays_per_probe = 256u;

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = validIndices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernelWithGrid(desc, validParams, reason),
               "grid-aware trace preflight succeeds for valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "grid-aware trace preflight reports no reject reason");
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectWithGrid(desc, validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectWithGrid none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchWithGrid(desc, validParams),
               "preflightProbeKernelLaunchWithGrid succeeds for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchWithGrid(desc, validParams),
               "wouldSkipProbeKernelLaunchWithGrid false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernelWithGrid(desc, validParams),
               "wouldSkipProbeTraceKernelWithGrid false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernelWithGrid(desc, validParams),
               "wouldSkipProbeBlendKernelWithGrid false for valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeBlendKernelWithGrid(desc, validParams),
               "canLaunchProbeBlendKernelWithGrid true for valid params");

    fuse::u32 oobIndices[2] = {0u, 99u};
    fuse::renderer::gi::DDGIKernelParams oobParams = validParams;
    oobParams.probe_indices_to_update = oobIndices;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernelWithGrid(desc, oobParams, reason),
               "grid-aware trace preflight rejects OOB probe indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex,
               "grid-aware trace preflight reports out_of_range_probe_index reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "out_of_range_probe_index") ==
                   0,
               "out_of_range_probe_index kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchWithGrid(desc, oobParams),
               "wouldSkipProbeKernelLaunchWithGrid true for OOB indices");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernelWithGrid(empty, validParams, reason),
               "grid-aware blend preflight rejects empty grid");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "grid-aware blend preflight reports empty_grid reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid kernel reject reason label");
}

void testDdgiDeepenPassGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {static_cast<fuse::f32>(i), 0.f, 0.f};
    }

    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc, cache.data(), 8u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for accessible grid");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc, cache.data(), 8u),
               "preflightProbeGridSource succeeds for accessible grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc, cache.data(), 8u),
               "wouldSkipProbeGridSource false for accessible grid");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::NullCache),
               "null_cache probe-grid source reject reason is blocking");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc, nullptr, 8u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::NullCache,
               "classifyProbeGridSourceReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc, nullptr, 8u),
               "wouldSkipProbeGridSource true for null cache");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::UndersizedCache),
                           "undersized_cache") == 0,
               "undersized_cache probe-grid source reject reason label");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen pass guards");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, built),
               "wouldSkipSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, hardOob),
               "wouldSkipSampleCoordPreflight true for hard OOB indices");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanTrilinearSampleAtProbeCoords(
                   desc, built, cache.data(), 8u, trilinearReason),
               "tryCanTrilinearSampleAtProbeCoords succeeds on accessible grid");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "valid trilinear preflight reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::canTrilinearSampleAtProbeCoords(desc, built, cache.data(), 8u),
               "canTrilinearSampleAtProbeCoords true for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid inputs");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "none trilinear reject reason is not blocking");

    fuse::renderer::ProbeSampleCoords clampableWeights = built;
    clampableWeights.tx = 2.f;
    expectTrue(fuse::renderer::ddgi_util::tryCanTrilinearSampleAtProbeCoords(
                   desc, clampableWeights, cache.data(), 8u, trilinearReason),
               "tryCanTrilinearSampleAtProbeCoords succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::ClampableWeights,
               "clampable weights report clampable_weights trilinear reason");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "clampable_weights trilinear reject reason is not blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(
                   desc, clampableWeights, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for clampable weights");
    expectTrue(std::strcmp(fuse::renderer::probeTrilinearSampleRejectReasonLabel(trilinearReason),
                           "clampable_weights") == 0,
               "clampable_weights trilinear reject reason label");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndexAtCoord(desc, cache.data(), validCoord, 8u, cacheReason),
               "tryValidateCacheIndexAtCoord succeeds for valid coord");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "valid coord cache-index reports no reject reason");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");

    const fuse::renderer::ProbeGridCoord oobCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexLookupCoord(desc, oobCoord),
               "wouldClampCacheIndexLookupCoord true for OOB coord");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), oobCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for clampable OOB coord");

    fuse::math::Vec3 coordIrradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtCoord(
                   desc, cache.data(), 8u, validCoord, coordIrradiance, cacheReason),
               "tryReadIrradianceAtCoord succeeds for valid coord");
    expectNear(coordIrradiance.x, 5.f, 1e-5f, "tryReadIrradianceAtCoord returns stored irradiance");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(empty, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true on empty grid");
}

void testDdgiWouldSkipPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::renderer::ProbeSampleCoordsRejectReason gridReason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeGridSource(empty, &gridReason),
               "preflightProbeGridSource rejects empty grid");
    expectTrue(gridReason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid source reports empty_grid reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeGridSource(zeroRes, &gridReason),
               "preflightProbeGridSource rejects zero irradiance_res grid");
    expectTrue(gridReason == fuse::renderer::ProbeSampleCoordsRejectReason::NotSampleableGrid,
               "zero irradiance_res source reports not_sampleable_grid reason");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldSkip preflight test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, built),
               "wouldSkipSampleCoordPreflight false for valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkipProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, oobWeights),
               "wouldSkipSampleCoordPreflight false for clampable weights");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, oobWeights),
               "wouldSkipProbeSampleCoords true for OOB weights");

    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, hardOob),
               "wouldSkipSampleCoordPreflight true for hard OOB indices");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, hardOob),
               "wouldSkipProbeSampleCoords true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for accessible grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, hardOob, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for invalid sample coords");

    const fuse::renderer::ProbeGridCoord coord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, coord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for in-range coord");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), coord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for in-range coord with cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, coord, 4u),
               "wouldSkipCacheIndexLookupAtCoord true for undersized cache");
    const fuse::renderer::ProbeGridCoord invalid{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, invalid, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");

    fuse::u32 validKernelIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validKernelIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeTraceKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeBlendKernelReject none for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeTraceKernelReject zero_rays_per_probe");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeBlendKernelReject zero_rays_per_probe");
}

void testDdgiDeepenFollowUpGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen follow-up guards");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, hardOob),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::cacheMatchesDesc(desc, 8u),
               "cacheMatchesDesc true when cache count matches probe count");
    expectTrue(!fuse::renderer::ddgi_util::cacheMatchesDesc(desc, 4u),
               "cacheMatchesDesc false when cache count is short");

    fuse::renderer::ProbeGridSourceRejectReason sourceReason =
        fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(desc, cache.data(), 8u, sourceReason),
               "tryValidateProbeGridSource succeeds for accessible grid");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "accessible grid source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(sourceReason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(sourceReason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc, cache.data(), 8u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for accessible grid");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc, cache.data(), 8u),
               "preflightProbeGridSource succeeds for accessible grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc, cache.data(), 8u),
               "wouldSkipProbeGridSource false for accessible grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc, nullptr, 8u),
               "wouldSkipProbeGridSource true for null cache");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc, nullptr, 8u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::NullCache,
               "classifyProbeGridSourceReject null cache");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::NullCache),
               "null_cache probe-grid source reject reason is blocking");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeLookupAtIndex(desc, cache.data(), 3u, 8u),
               "wouldSkipProbeLookupAtIndex false for in-range index");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeLookupAtIndex(desc, cache.data(), 99u, 8u),
               "wouldSkipProbeLookupAtIndex true for OOB index");
    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipProbeLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeLookupAtCoord(desc, cache.data(), invalidCoord, 8u),
               "wouldSkipProbeLookupAtCoord true for invalid coord");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanTrilinearSampleAtProbeCoords(
                   desc, built, cache.data(), 8u, trilinearReason),
               "tryCanTrilinearSampleAtProbeCoords succeeds on accessible grid");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "valid trilinear preflight reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::canTrilinearSampleAtProbeCoords(desc, built, cache.data(), 8u),
               "canTrilinearSampleAtProbeCoords true for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid coords");

    fuse::renderer::ProbeSampleCoords warnWeights = built;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::ddgi_util::tryCanTrilinearSampleAtProbeCoords(
                   desc, warnWeights, cache.data(), 8u, trilinearReason),
               "tryCanTrilinearSampleAtProbeCoords succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::ClampableWeights,
               "clampable weights report clampable_weights trilinear reason");
    expectTrue(std::strcmp(fuse::renderer::probeTrilinearSampleRejectReasonLabel(trilinearReason),
                           "clampable_weights") == 0,
               "clampable_weights trilinear reject reason label");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "clampable_weights trilinear reject reason is not blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, warnWeights, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for clampable weights");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, hardOob, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample true for hard OOB indices");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "tryScheduleProbeUpdatesAtRate zero rate reports zero_probes_per_frame reason");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(empty, cache.data(), 8u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty grid");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(zeroRes, cache.data(), 8u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "classifyProbeGridSourceReject not sampleable grid");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;
    desc.depth_res = 16u;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init, reason),
               "valid desc passes init source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid init source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "preflightProbeGridSource succeeds for valid init desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "wouldSkip false for valid init desc");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   empty, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid for init");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   empty, fuse::renderer::ProbeGridSourceKind::Sample, reason),
               "empty grid fails sample source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid sample source reports empty_grid reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroIrradiance, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res for init");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroDepth, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res for init");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   badSpacing, fuse::renderer::ProbeGridSourceKind::Sample) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing for sample");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRays, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe for update");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe),
                           "zero_rays_per_probe") == 0,
               "zero_rays_per_probe probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRate = desc;
    zeroRate.probes_per_frame = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRate, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroProbesPerFrame,
               "classifyProbeGridSourceReject zero_probes_per_frame for update");
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Update, reason),
               "valid desc passes update source validation");
}

void testTrilinearSamplePreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, nullptr, 8u, &reason),
               "preflightTrilinearProbeSample rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache trilinear preflight reports null_cache reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
}

void testScheduledCacheIndexGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 7u};
    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(
                   desc, cache.data(), validIndices, 2u, 8u, reason),
               "tryValidateScheduledCacheIndices succeeds for valid indices");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid scheduled cache indices report no reject reason");
    expectTrue(fuse::renderer::ddgi_util::preflightScheduledCacheIndices(
                   desc, cache.data(), validIndices, 2u, 8u),
               "preflightScheduledCacheIndices succeeds for valid indices");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipScheduledCacheIndices(
                   desc, cache.data(), validIndices, 2u, 8u),
               "wouldSkipScheduledCacheIndices false for valid indices");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(
                   desc, cache.data(), oobIndices, 2u, 8u, reason),
               "tryValidateScheduledCacheIndices rejects OOB index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB scheduled index reports out_of_range_probe_index reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipScheduledCacheIndices(
                   desc, cache.data(), oobIndices, 2u, 8u),
               "wouldSkipScheduledCacheIndices true for OOB index");

    expectTrue(fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(desc, validIndices, 0u, 8u, reason),
               "zero-count scheduled cache validation vacuously succeeds");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateScheduledCacheIndices(
                   desc, nullptr, validIndices, 2u, 8u, reason),
               "null cache fails scheduled cache validation");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache scheduled validation reports null_cache reason");
}

void testScheduleAtRateDeepen() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(2048u, 64u, 64u) == 64u,
               "effectiveScheduledProbeCount matches batch size");
    expectTrue(fuse::renderer::ddgi_util::effectiveScheduledProbeCount(2048u, 64u, 32u) == 32u,
               "effectiveScheduledProbeCount capped by max_indices");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule-at-rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelDescPreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectForDesc none for valid desc+params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchForDesc(desc, kernelParams),
               "preflightProbeKernelLaunchForDesc succeeds for valid desc+params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc false for valid desc+params");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(empty, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelRejectForDesc empty_grid for empty desc");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(empty, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc true for empty desc");

    fuse::u32 oobIndices[2] = {0u, 99u};
    fuse::renderer::gi::DDGIKernelParams oobParams = kernelParams;
    oobParams.probe_indices_to_update = oobIndices;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, oobParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex,
               "classifyProbeKernelRejectForDesc out_of_range_probe_index for OOB indices");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex),
                           "out_of_range_probe_index") == 0,
               "out_of_range_probe_index kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, oobParams),
               "wouldSkipProbeKernelLaunchForDesc true for OOB indices");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelRejectForDesc zero_rays_per_probe after empty-grid check");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;
    desc.depth_res = 16u;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init, reason),
               "valid desc passes init source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid init source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "preflightProbeGridSource succeeds for valid init desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "wouldSkipProbeGridSource false for valid init desc");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   empty, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid for init");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   empty, fuse::renderer::ProbeGridSourceKind::Sample, reason),
               "empty grid fails sample source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid sample source reports empty_grid reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroIrradiance, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res for init");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroDepth, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res for init");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   badSpacing, fuse::renderer::ProbeGridSourceKind::Sample) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing for sample");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRays, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe for update");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe),
                           "zero_rays_per_probe") == 0,
               "zero_rays_per_probe probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRate = desc;
    zeroRate.probes_per_frame = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRate, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroProbesPerFrame,
               "classifyProbeGridSourceReject zero_probes_per_frame for update");
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Update, reason),
               "valid desc passes update source validation");
}

void testDdgiTrilinearPreflightDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, nullptr, 8u),
               "preflightTrilinearProbeSample rejects null cache");

    fuse::renderer::ProbeSampleCoords invalid = built;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for unordered corners");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), invalidCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, nullptr, validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for null cache");
}

void testScheduleAtRateDeepen() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule-at-rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelDescPreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectForDesc none for valid desc+params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchForDesc(desc, kernelParams),
               "preflightProbeKernelLaunchForDesc succeeds for valid desc+params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc false for valid desc+params");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(empty, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelRejectForDesc empty_grid for empty desc");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(empty, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc true for empty desc");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelRejectForDesc zero_rays_per_probe after empty-grid check");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;
    desc.depth_res = 16u;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init, reason),
               "valid desc passes init source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid init source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "preflightProbeGridSource succeeds for valid init desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "wouldSkipProbeGridSource false for valid init desc");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   empty, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid for init");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   empty, fuse::renderer::ProbeGridSourceKind::Sample, reason),
               "empty grid fails sample source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid sample source reports empty_grid reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroIrradiance, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res for init");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroDepth, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res for init");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   badSpacing, fuse::renderer::ProbeGridSourceKind::Sample) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing for sample");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRays, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe for update");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe),
                           "zero_rays_per_probe") == 0,
               "zero_rays_per_probe probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRate = desc;
    zeroRate.probes_per_frame = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRate, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroProbesPerFrame,
               "classifyProbeGridSourceReject zero_probes_per_frame for update");
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Update, reason),
               "valid desc passes update source validation");
}

void testDdgiTrilinearPreflightDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, nullptr, 8u),
               "preflightTrilinearProbeSample rejects null cache");

    fuse::renderer::ProbeSampleCoords invalid = built;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for unordered corners");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), invalidCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, nullptr, validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for null cache");
}

void testScheduleAtRateDeepen() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule-at-rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelDescPreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectForDesc none for valid desc+params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchForDesc(desc, kernelParams),
               "preflightProbeKernelLaunchForDesc succeeds for valid desc+params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc false for valid desc+params");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(empty, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelRejectForDesc empty_grid for empty desc");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(empty, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc true for empty desc");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelRejectForDesc zero_rays_per_probe after empty-grid check");
}

void testProbeGridSourcePreflights() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::classifyProbeGridSourceReject(desc) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for sampleable grid");
    expectTrue(fuse::renderer::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");
    expectTrue(!fuse::renderer::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe-grid source reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(empty) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid probe-grid source reject reason label");
    expectTrue(fuse::renderer::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(zeroRes) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(badSpacing) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
               "invalid_spacing probe-grid source reject reason is blocking");
}

void testProbeTrilinearSamplePreflights() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::ProbeSampleCoords preflightCoords{};
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, &preflightCoords),
               "world-position trilinear preflight succeeds");
    expectTrue(preflightCoords.x0 == 0u && preflightCoords.x1 == 1u,
               "world-position trilinear preflight returns built coords");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null_cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::renderer::ProbeSampleCoords emptyCoords{};
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeTrilinearSample(
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u, &emptyCoords),
               "world-position trilinear preflight rejects empty grid");
}

void testWouldSkipSampleCoordPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldSkip sample-coord preflight");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, built),
               "wouldSkipSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, oobWeights),
               "wouldSkipSampleCoordPreflight false for clampable weights");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, oobIndices),
               "wouldSkipSampleCoordPreflight true for hard OOB indices");
}

void testProbeScheduleAtRatePreflights() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testCacheIndexPreflightOverload() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "count-only preflightCacheIndexLookup succeeds for valid index");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "count-only preflightCacheIndexLookup rejects OOB index");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeLookup(desc, cache.data(), 8u),
               "wouldSkipProbeLookup false for accessible cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeLookup(desc, nullptr, 8u),
               "wouldSkipProbeLookup true for null cache");
}

void testDdgiDeepenFollowUpGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for sampleable grid");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe grid source reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(empty) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid probe grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(zeroRes) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
               "zero_irradiance_res probe grid source reject reason is blocking");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(badSpacing) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe grid source reject reason label");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen follow-up sample-coord test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordsPreflight(desc, built),
               "wouldSkipProbeSampleCoordsPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordsPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordsPreflight true for hard OOB indices");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordsPreflight(desc, oobWeights),
               "wouldSkipProbeSampleCoordsPreflight false for clampable weights");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, built, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "count-only preflightCacheIndexLookup succeeds for valid index");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "count-only preflightCacheIndexLookup rejects OOB index");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    const fuse::renderer::ProbeGridSource source =
        fuse::renderer::ProbeGridSource::fromDescAndCache(desc, cache.data(), 8u);

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(source, reason),
               "accessible probe-grid source passes validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid probe-grid source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(source),
               "preflightProbeGridSource succeeds for accessible source");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(source),
               "wouldSkipProbeGridSource false for accessible source");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(source) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for accessible source");

    const fuse::renderer::ProbeGridSource nullCache =
        fuse::renderer::ProbeGridSource::fromDescAndCache(desc, nullptr, 8u);
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(nullCache, reason),
               "null cache fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::NullCache,
               "null cache reports null_cache source reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "null_cache source reject reason is blocking");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(nullCache),
               "wouldSkipProbeGridSource true for null cache");

    const fuse::renderer::ProbeGridSource undersized =
        fuse::renderer::ProbeGridSource::fromDescAndCache(desc, cache.data(), 4u);
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(undersized, reason),
               "undersized cache fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache source reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "undersized_cache") == 0,
               "undersized_cache probe-grid source reject reason label");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    const fuse::renderer::ProbeGridSource emptySource =
        fuse::renderer::ProbeGridSource::fromDescAndCache(empty, cache.data(), 8u);
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(emptySource, reason),
               "empty grid fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid reports empty_grid source reason");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    const fuse::renderer::ProbeGridSource notSampleable =
        fuse::renderer::ProbeGridSource::fromDescAndCache(zeroRes, cache.data(), 8u);
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(notSampleable, reason),
               "non-sampleable grid fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "zero irradiance_res reports not_sampleable source reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "not_sampleable") == 0,
               "not_sampleable probe-grid source reject reason label");
}

void testTrilinearSamplePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSampleAtCoords(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSampleAtCoords succeeds for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSampleAtCoords false for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid world position");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSampleAtCoords true for null cache");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null_cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(
                   desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSampleAtCoords true for unordered corners");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");

    fuse::renderer::ProbeSampleCoords oobWeights = coords;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobWeights),
               "wouldSkipProbeSampleCoordPreflight false for clampable weights");

    fuse::renderer::ProbeSampleCoords hardOob = coords;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, hardOob),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, invalid),
               "wouldSkipProbeSampleCoordPreflight false for non-blocking unordered corners");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, coords),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
}

void testDdgiKernelUpdatePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::preflightDdgiKernelUpdate(desc, validIndices, 2u, 42u, &reason),
               "preflightDdgiKernelUpdate succeeds for valid indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "valid kernel update preflight reports no reject reason");
    expectTrue(!fuse::renderer::gi::wouldSkipDdgiKernelUpdate(desc, validIndices, 2u, 42u),
               "wouldSkipDdgiKernelUpdate false for valid indices");

    expectTrue(fuse::renderer::gi::wouldSkipDdgiKernelUpdate(desc, nullptr, 2u),
               "wouldSkipDdgiKernelUpdate true for null indices");
    expectTrue(fuse::renderer::gi::wouldSkipDdgiKernelUpdate(desc, validIndices, 0u),
               "wouldSkipDdgiKernelUpdate true for zero count");
}

void testProbeScheduleAtRatePreflight() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8u;
    desc.depth_res = 16u;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init, reason),
               "valid desc passes init source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid init source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "preflightProbeGridSource succeeds for valid init desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Init),
               "wouldSkipProbeGridSource false for valid init desc");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   empty, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid for init");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   empty, fuse::renderer::ProbeGridSourceKind::Sample, reason),
               "empty grid fails sample source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid sample source reports empty_grid reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "empty_grid probe-grid source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroIrradiance, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res for init");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroDepth, fuse::renderer::ProbeGridSourceKind::Init) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res for init");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   badSpacing, fuse::renderer::ProbeGridSourceKind::Sample) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing for sample");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRays, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe for update");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe),
                           "zero_rays_per_probe") == 0,
               "zero_rays_per_probe probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroRate = desc;
    zeroRate.probes_per_frame = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(
                   zeroRate, fuse::renderer::ProbeGridSourceKind::Update) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroProbesPerFrame,
               "classifyProbeGridSourceReject zero_probes_per_frame for update");
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(
                   desc, fuse::renderer::ProbeGridSourceKind::Update, reason),
               "valid desc passes update source validation");
}

void testDdgiTrilinearPreflightDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for trilinear preflight deepen test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, nullptr, 8u),
               "preflightTrilinearProbeSample rejects null cache");

    fuse::renderer::ProbeSampleCoords invalid = built;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for unordered corners");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), invalidCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for invalid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, nullptr, validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for null cache");
}

void testScheduleAtRateDeepen() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule-at-rate reports zero_probes_per_frame reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testKernelDescPreflightDeepen() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelRejectForDesc none for valid desc+params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunchForDesc(desc, kernelParams),
               "preflightProbeKernelLaunchForDesc succeeds for valid desc+params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(desc, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc false for valid desc+params");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(empty, kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "classifyProbeKernelRejectForDesc empty_grid for empty desc");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                               fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunchForDesc(empty, kernelParams),
               "wouldSkipProbeKernelLaunchForDesc true for empty desc");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelRejectForDesc(desc, zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelRejectForDesc zero_rays_per_probe after empty-grid check");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;
    desc.depth_res = 16;
    desc.rays_per_probe = 256;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::tryValidateProbeGridSource(desc, reason),
               "valid probe grid source passes tryValidate");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid probe grid source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(fuse::renderer::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for valid descriptor");
    expectTrue(!fuse::renderer::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for valid descriptor");
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(desc) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for valid descriptor");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe-grid source reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::tryValidateProbeGridSource(empty, reason),
               "empty grid fails probe grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid reports empty_grid source reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid probe-grid source reject reason label");
    expectTrue(fuse::renderer::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(badSpacing) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
               "invalid_spacing source reject reason is blocking");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(zeroIrradiance) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(zeroDepth) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "classifyProbeGridSourceReject zero_depth_res");

    fuse::renderer::DDGIDesc zeroRays = desc;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(zeroRays) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroRaysPerProbe,
               "classifyProbeGridSourceReject zero_rays_per_probe");
}

void testDdgiDeepenPassGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for deepen pass guards");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, coords),
               "wouldSkipProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = coords;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, oobIndices),
               "wouldSkipProbeSampleCoords true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "count-only preflightCacheIndexLookup succeeds");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 3u, 8u),
               "wouldSkipReadIrradianceAtIndex false for valid cache read");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, nullptr, 3u, 8u),
               "wouldSkipReadIrradianceAtIndex true for null cache");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");

    fuse::renderer::DDGISampleRequest request{};
    request.world_position = {0.5f, 0.5f, 0.5f};
    request.world_normal = {0.f, 1.f, 0.f};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSample(desc, request, cache.data(), 8u),
               "wouldSkipProbeSample false for valid request");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSample(desc, request, cache.data(), 4u),
               "wouldSkipProbeSample true for undersized cache");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams populated{};
    expectTrue(fuse::renderer::gi::preflightPopulatedProbeKernelLaunch(
                   populated, desc, validIndices, 2u, 99u),
               "preflightPopulatedProbeKernelLaunch succeeds for valid params");
    expectTrue(populated.frame_seed == 99u, "preflightPopulatedProbeKernelLaunch sets frame_seed");
}

void testDdgiDeepenPassGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::classifyProbeGridSourceReject(desc) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for sampleable grid");
    expectTrue(fuse::renderer::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");
    expectTrue(!fuse::renderer::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe-grid source reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(empty) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid probe-grid source reject reason label");
    expectTrue(fuse::renderer::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::classifyProbeGridSourceReject(zeroRes) ==
                   fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "classifyProbeGridSourceReject not_sampleable for zero irradiance_res");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::NotSampleable),
                           "not_sampleable") == 0,
               "not_sampleable probe-grid source reject reason label");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::NotSampleable),
               "not_sampleable probe-grid source reject reason is blocking");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen pass guard test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, hardOob),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearProbeSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "preflightTrilinearProbeIrradiance succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "wouldSkipTrilinearProbeIrradiance false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 8u),
               "preflightTrilinearDirectionalProbeIrradiance succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 8u),
               "wouldSkipTrilinearDirectionalProbeIrradiance false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u),
               "wouldSkipTrilinearProbeIrradiance true for null cache");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(kernelParams),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(kernelParams),
               "preflightProbeBlendKernel succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroRays, &kernelReason),
               "preflightProbeTraceKernel rejects zero rays");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "preflightProbeTraceKernel reports zero_rays_per_probe reason");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(desc, reason),
               "default desc passes probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid probe-grid source reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for valid desc");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for valid desc");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "none probe-grid source reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(empty, reason),
               "empty grid fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid reports empty_grid source reason");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(empty) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");

    fuse::renderer::DDGIDesc zeroIrradiance = desc;
    zeroIrradiance.irradiance_res = 0u;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(zeroIrradiance, reason),
               "zero irradiance_res fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "zero irradiance_res reports zero_irradiance_res source reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid source reject reason label");

    fuse::renderer::DDGIDesc zeroDepth = desc;
    zeroDepth.depth_res = 0u;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(zeroDepth, reason),
               "zero depth_res fails probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::ZeroDepthRes,
               "zero depth_res reports zero_depth_res source reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(reason),
               "zero_depth_res probe-grid source reject reason is blocking");
}

void testTrilinearSamplePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds on accessible grid");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(reason),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for invalid sample coords");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid sample coords");
}

void testProbeScheduleAtRateDeepenGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "successful rate-aware schedule reports no reject reason");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 0u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "tryScheduleProbeUpdatesAtRate zero probe count reports zero_probe_count reason");
}

void testNotSampleableGridBlocking() {
    expectTrue(fuse::renderer::probeSampleCoordsRejectReasonIsBlocking(
                   fuse::renderer::ProbeSampleCoordsRejectReason::NotSampleableGrid),
               "not_sampleable_grid sample-coord reject reason is blocking");
}

void testDdgiDeepenPassGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for sampleable grid");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe-grid source reject reason is not blocking");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::NotSampleable),
               "not_sampleable probe-grid source reject reason is blocking");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::NotSampleable),
                           "not_sampleable") == 0,
               "not_sampleable probe-grid source reject reason label");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(empty) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(zeroRes) ==
                   fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "classifyProbeGridSourceReject not_sampleable for zero irradiance_res");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen-pass sample-coord skip test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, built, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");

    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexRejectAtCoord(desc, cache.data(), validCoord, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classifyCacheIndexRejectAtCoord none for valid coord");
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "preflightCacheIndexLookupAtCoord succeeds for valid coord");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), validCoord, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexCoordForLookup(desc, validCoord),
               "wouldClampCacheIndexCoordForLookup false for in-range coord");

    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexCoordForLookup(desc, invalidCoord),
               "wouldClampCacheIndexCoordForLookup true for OOB coord");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexRejectAtCoord(desc, cache.data(), invalidCoord, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "classifyCacheIndexRejectAtCoord out_of_range_probe_index");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    const fuse::renderer::ProbeGridSource descSource =
        fuse::renderer::ddgi_util::probeGridSourceFromDesc(desc);
    expectTrue(descSource.kind == fuse::renderer::ProbeGridSourceKind::Desc,
               "probeGridSourceFromDesc sets Desc kind");
    fuse::renderer::ProbeGridSourceRejectReason sourceReason =
        fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(descSource, sourceReason),
               "desc-only probe grid source validates");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "desc-only source reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(descSource),
               "preflightProbeGridSource succeeds for desc-only source");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(descSource),
               "wouldSkip false for valid desc-only source");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(sourceReason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(sourceReason),
               "none probe-grid source reject reason is not blocking");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    const fuse::renderer::ProbeGridSource cacheSource =
        fuse::renderer::ddgi_util::probeGridSourceFromCache(desc, cache.data(), 8u);
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSourceForSampling(cacheSource, sourceReason),
               "cpu-cache source validates for sampling");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(cacheSource) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for full cache");

    const fuse::renderer::ProbeGridSource nullCacheSource =
        fuse::renderer::ddgi_util::probeGridSourceFromCache(desc, nullptr, 8u);
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(nullCacheSource, sourceReason),
               "null cpu cache fails source validation");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::NullCache,
               "null cache reports null_cache source reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(nullCacheSource),
               "wouldSkip true for null cache source");

    const fuse::renderer::ProbeGridSource undersizedSource =
        fuse::renderer::ddgi_util::probeGridSourceFromCache(desc, cache.data(), 4u);
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(undersizedSource, sourceReason),
               "undersized cpu cache fails source validation");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache source reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(sourceReason), "undersized_cache") == 0,
               "undersized_cache probe-grid source reject reason label");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(sourceReason),
               "undersized_cache source reject reason is blocking");

    fuse::renderer::ProbeData probeData{};
    probeData.desc = desc;
    const fuse::renderer::ProbeGridSource probeDataSource =
        fuse::renderer::ddgi_util::probeGridSourceFromProbeData(probeData);
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(probeDataSource, sourceReason),
               "probe-data source validates when desc matches");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(probeDataSource),
               "preflightProbeGridSource succeeds for probe-data source");

    fuse::renderer::DDGIDesc differentDesc = desc;
    differentDesc.grid_dims.x = 4u;
    fuse::renderer::ProbeGridSource mismatchSource =
        fuse::renderer::ddgi_util::probeGridSourceFromProbeData(probeData);
    mismatchSource.desc = &differentDesc;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(mismatchSource, sourceReason),
               "probe-data source rejects external desc mismatch");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::DescProbeDataMismatch,
               "desc mismatch reports desc_probe_data_mismatch reason");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    const fuse::renderer::ProbeGridSource emptySource =
        fuse::renderer::ddgi_util::probeGridSourceFromDesc(empty);
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(emptySource, sourceReason),
               "empty grid fails source validation");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "empty grid reports empty_grid source reason");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    const fuse::renderer::ProbeGridSource zeroResSource =
        fuse::renderer::ddgi_util::probeGridSourceFromDesc(zeroRes);
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(zeroResSource, sourceReason),
               "zero irradiance_res still valid for desc-only source");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSourceForSampling(zeroResSource, sourceReason),
               "zero irradiance_res fails sampling source validation");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "zero irradiance_res reports not_sampleable source reason");
}

void testTrilinearSamplePreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkip false for valid trilinear sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
               "wouldSkip true for null cache trilinear sample");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid sample coords");

    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, invalid),
               "wouldSkipProbeSampleCoords true for unordered corners");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, coords),
               "wouldSkipProbeSampleCoords false for valid coords");

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "preflightCacheIndexLookup succeeds without cache pointer");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "preflightCacheIndexLookup rejects OOB index without cache pointer");
}

void testScheduleAtRatePreflightGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero probes per frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes per frame");
}

void testKernelPreflightGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams params{};
    params.probe_indices_to_update = indices;
    params.probe_update_count = 2u;
    params.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(params),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(params),
               "preflightProbeBlendKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(params) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeBlendKernelReject none for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = params;
    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroCount),
               "preflightProbeTraceKernel rejects zero update count");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(zeroCount),
               "preflightProbeBlendKernel rejects zero update count");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(zeroCount) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "classifyProbeBlendKernelReject zero update count");
}

void testDdgiDeepenPassGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridReject(desc) ==
                   fuse::renderer::ProbeGridRejectReason::None,
               "classifyProbeGridReject none for sampleable grid");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGrid(desc),
               "preflightProbeGrid succeeds for sampleable grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGrid(desc),
               "wouldSkipProbeGrid false for sampleable grid");
    expectTrue(!fuse::renderer::probeGridRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridRejectReason::None),
               "none probe-grid reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridReject(empty) ==
                   fuse::renderer::ProbeGridRejectReason::EmptyGrid,
               "classifyProbeGridReject empty_grid");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(
                               fuse::renderer::ProbeGridRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid probe-grid reject reason label");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGrid(empty),
               "wouldSkipProbeGrid true for empty grid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridReject(zeroRes) ==
                   fuse::renderer::ProbeGridRejectReason::ZeroIrradianceRes,
               "classifyProbeGridReject zero_irradiance_res");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(
                               fuse::renderer::ProbeGridRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res probe-grid reject reason label");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridReject(badSpacing) ==
                   fuse::renderer::ProbeGridRejectReason::InvalidSpacing,
               "classifyProbeGridReject invalid_spacing");
    expectTrue(std::strcmp(fuse::renderer::probeGridRejectReasonLabel(
                               fuse::renderer::ProbeGridRejectReason::InvalidSpacing),
                           "invalid_spacing") == 0,
               "invalid_spacing probe-grid reject reason label");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexSource(desc, 3u) ==
                   fuse::renderer::ProbeGridSource::FlatIndex,
               "in-range probe index classified as flat_index");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexSource(desc, 99u) ==
                   fuse::renderer::ProbeGridSource::ClampedIndex,
               "OOB probe index classified as clamped_index");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceLabel(
                               fuse::renderer::ProbeGridSource::FlatIndex),
                           "flat_index") == 0,
               "flat_index probe-grid source label");

    const fuse::renderer::ProbeGridCoord validCoord{1u, 1u, 1u};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordSource(desc, validCoord) ==
                   fuse::renderer::ProbeGridSource::GridCoord,
               "valid coord classified as grid_coord");
    const fuse::renderer::ProbeGridCoord invalidCoord{9u, 9u, 9u};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordSource(desc, invalidCoord) ==
                   fuse::renderer::ProbeGridSource::ClampedGridCoord,
               "OOB coord classified as clamped_grid_coord");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceLabel(
                               fuse::renderer::ProbeGridSource::WorldPosition),
                           "world_position") == 0,
               "world_position probe-grid source label");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen-pass sample preflight");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, hardOob),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "count-only preflightCacheIndexLookup succeeds for valid index");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "count-only preflightCacheIndexLookup rejects OOB index");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeTrilinearSample(
                   desc, built, cache.data(), 8u, trilinearReason),
               "tryPreflightProbeTrilinearSample succeeds on accessible grid");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "valid trilinear preflight reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, built, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds on accessible grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid coords");

    fuse::renderer::ProbeSampleCoords warnWeights = built;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeTrilinearSample(
                   desc, warnWeights, cache.data(), 8u, trilinearReason),
               "tryPreflightProbeTrilinearSample warns but succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::ClampableWeights,
               "clampable weights report clampable_weights trilinear reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeTrilinearSampleRejectReasonLabel(trilinearReason),
                           "clampable_weights") == 0,
               "clampable_weights trilinear reject reason label");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "clampable_weights is not blocking for trilinear preflight");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(
                   desc, warnWeights, cache.data(), 8u),
               "clampable weights do not skip trilinear sample");

    expectTrue(!fuse::renderer::ddgi_util::tryPreflightProbeTrilinearSample(
                   desc, hardOob, cache.data(), 8u, trilinearReason),
               "tryPreflightProbeTrilinearSample rejects hard OOB indices");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "hard OOB indices report invalid_sample_coords trilinear reject reason");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "invalid_sample_coords is blocking for trilinear preflight");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, hardOob, cache.data(), 8u),
               "hard OOB coords skip trilinear sample");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, hardOob, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid_sample_coords");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testDdgiPreflightDeepenGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeSchedule(2048u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeSchedule succeeds for valid inputs");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "tryPreflightProbeSchedule reports none for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "shouldSkipProbeSchedule false for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeSchedule(0u, 64u, indices, &count),
               "shouldSkipProbeSchedule true for zero probe count");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    cache[3u].irradiance = {0.25f, 0.5f, 0.75f};
    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightCacheIndexLookup(desc, cache.data(), 3u, 8u, cacheReason),
               "tryPreflightCacheIndexLookup succeeds for valid cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "tryPreflightCacheIndexLookup reports none for valid cache");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, cache.data(), 3u, 8u),
               "shouldSkipCacheIndexLookup false for valid cache");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u),
               "shouldSkipCacheIndexLookup true for null cache");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexRejectAtCoord(desc, cache.data(), 1u, 1u, 0u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classifyCacheIndexRejectAtCoord none for valid coord");
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexCoord(desc, 9u, 0u, 0u),
               "wouldClampCacheIndexCoord true for OOB x");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexCoord(desc, 1u, 0u, 0u),
               "wouldClampCacheIndexCoord false for in-range coord");

    fuse::math::Vec3 coordIrradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtCoord(
                   desc, cache.data(), 8u, 1u, 1u, 0u, coordIrradiance, cacheReason),
               "tryReadIrradianceAtCoord succeeds for valid coord");
    expectNear(coordIrradiance.x, cache[3u].irradiance.x, 1e-5f, "tryReadIrradianceAtCoord returns stored irradiance");

    fuse::math::Vec3 indexIrradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, cache.data(), 8u, 3u, indexIrradiance, cacheReason),
               "tryReadIrradianceAtIndex with reason succeeds for valid index");
               "tryReadIrradianceAtIndex with reason reports none");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for third-pass sample-coord guard test");
    expectTrue(!fuse::renderer::ProbeGridLayout::shouldSkipProbeSampleCoords(desc, built),
               "shouldSkipProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTrilinearProbeSample(desc, built, cache.data(), 8u, trilinearReason),
               "tryPreflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "tryPreflightTrilinearProbeSample reports none");
    expectTrue(!fuse::renderer::shouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "shouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(fuse::renderer::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(fuse::renderer::shouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "shouldSkipTrilinearProbeSample true for null cache");

    expectTrue(fuse::renderer::ddgi_util::tryCanSampleAtProbeCoord(desc, 1u, 0u, 0u, cache.data(), 8u, trilinearReason),
               "tryCanSampleAtProbeCoord succeeds for valid coord");
               "tryCanSampleAtProbeCoord reports none");

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightDdgiProbeUpdate(desc, validIndices, 2u, launchReason),
               "tryPreflightDdgiProbeUpdate succeeds for valid launch");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "tryPreflightDdgiProbeUpdate reports none");
    expectTrue(!fuse::renderer::shouldSkipDdgiProbeUpdate(desc, validIndices, 2u),
               "shouldSkipDdgiProbeUpdate false for valid launch");

    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryPreflightProbeKernelLaunch(kernelParams, kernelReason),
               "tryPreflightProbeKernelLaunch succeeds for valid params");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "tryPreflightProbeKernelLaunch reports none");
    expectTrue(!fuse::renderer::gi::shouldSkipProbeKernelLaunch(kernelParams),
               "shouldSkipProbeKernelLaunch false for valid params");
}

void testDdgiPreflightDeepenGuards() {

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleReject none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeSchedule(2048u, 64u, indices, &count),
               "preflightProbeSchedule succeeds for valid inputs");
    expectTrue(!fuse::renderer::probeScheduleRejectReasonIsBlocking(
                   fuse::renderer::ProbeScheduleRejectReason::None),
               "none schedule reject reason is not blocking");
    expectTrue(fuse::renderer::probeScheduleRejectReasonIsBlocking(
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount),
               "zero_probe_count schedule reject reason is blocking");
    expectTrue(fuse::renderer::ddgi_util::wouldClampScheduledProbeCount(2048u, 64u, 32u),
               "wouldClampScheduledProbeCount true when max_indices caps batch");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampScheduledProbeCount(2048u, 64u, 64u),
               "wouldClampScheduledProbeCount false when capacity matches batch");

    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classifyCacheIndexReject none for valid index");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, cache.data(), 3u, 8u) ==
               "classifyCacheIndexReject none for valid cache pointer");
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, cache.data(), 3u, 8u),
               "preflightCacheIndexLookup succeeds for valid cache");
    expectTrue(!fuse::renderer::cacheIndexRejectReasonIsBlocking(
                   fuse::renderer::CacheIndexRejectReason::None),
               "none cache-index reject reason is not blocking");
    expectTrue(fuse::renderer::cacheIndexRejectReasonIsBlocking(
                   fuse::renderer::CacheIndexRejectReason::NullCache),
               "null_cache reject reason is blocking");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::NullCache,
               "classifyCacheIndexReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldClampProbeIndexForLookup(99u, desc),
               "wouldClampProbeIndexForLookup true for OOB index");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampProbeIndexForLookup(3u, desc),
               "wouldClampProbeIndexForLookup false for in-range index");

               "build coords for preflight deepen test");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classifyProbeSampleCoordsReject none for valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "canPreflightProbeSampleCoords true for valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "wouldClampProbeSampleCoords false for valid coords");

    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, oobWeights),
               "wouldClampProbeSampleCoords true for OOB weights");
    fuse::renderer::ProbeSampleCoordsRejectReason sampleReason =
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, oobWeights, sampleReason),
               "tryPreflightProbeSampleCoords succeeds for clampable weights");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "clampable weights report out_of_range_weights reason");
    expectTrue(!fuse::renderer::probeSampleCoordsRejectReasonIsBlocking(sampleReason),
               "out_of_range_weights is not blocking for preflight");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, oobIndices) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "classifyProbeSampleCoordsReject out_of_range_indices");
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, oobIndices, sampleReason),
               "tryPreflightProbeSampleCoords rejects hard OOB indices");
    expectTrue(fuse::renderer::probeSampleCoordsRejectReasonIsBlocking(sampleReason),
               "out_of_range_indices is blocking for preflight");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(
                   desc, {0.5f, 0.5f, 0.5f}, &preflightCoords),
               "preflightProbeSampleCoords succeeds for interior sample");
    expectTrue(preflightCoords.x0 == 0u && preflightCoords.x1 == 1u,
               "preflightProbeSampleCoords returns built coords");

    expectTrue(fuse::renderer::classifyDdgiProbeUpdateReject(desc, validIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "classifyDdgiProbeUpdateReject none for valid launch");
    expectTrue(fuse::renderer::preflightDdgiProbeUpdate(desc, validIndices, 2u),
               "preflightDdgiProbeUpdate succeeds for valid launch");
    expectTrue(!fuse::renderer::probeUpdateLaunchRejectReasonIsBlocking(
                   fuse::renderer::ProbeUpdateLaunchRejectReason::None),
               "none launch reject reason is not blocking");

    fuse::u32 oobLaunchIndices[2] = {0u, 99u};
    expectTrue(fuse::renderer::classifyDdgiProbeUpdateReject(desc, oobLaunchIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "classifyDdgiProbeUpdateReject out_of_range_probe_index");

    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunch(kernelParams),
               "preflightProbeKernelLaunch succeeds for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunch(kernelParams),
               "wouldSkipProbeKernelLaunch false for valid params");
    expectTrue(!fuse::renderer::gi::probeKernelRejectReasonIsBlocking(
                   fuse::renderer::gi::ProbeKernelRejectReason::None),
               "none kernel reject reason is not blocking");

    fuse::renderer::gi::DDGIKernelParams populated{};
    fuse::renderer::gi::populateDDGIKernelParams(populated, desc, validIndices, 2u, 42u);
    expectTrue(populated.probe_indices_to_update == validIndices, "populateDDGIKernelParams sets indices");
    expectTrue(populated.probe_update_count == 2u, "populateDDGIKernelParams sets update count");
    expectTrue(populated.rays_per_probe == desc.rays_per_probe, "populateDDGIKernelParams sets rays_per_probe");
    expectTrue(populated.frame_seed == 42u, "populateDDGIKernelParams sets frame_seed");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeKernelReject zero_rays_per_probe");
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeTraceKernelReject zero_rays_per_probe");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeBlendKernelReject zero_rays_per_probe");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunch(zeroRays),
               "wouldSkipProbeKernelLaunch true for zero rays");
    expectTrue(fuse::renderer::ddgi_util::isProbeIndexCacheAccessible(7u, 8u),
               "last cache index is accessible");
    expectTrue(!fuse::renderer::ddgi_util::isProbeIndexCacheAccessible(8u, 8u),
               "index equal to cache_count is not accessible");
    expectTrue(fuse::renderer::ddgi_util::isProbeIndexCacheOutOfRange(8u, 8u),
               "index equal to cache_count is out of range");
    expectTrue(fuse::renderer::ddgi_util::isProbeIndexCacheOutOfRange(0u, 0u),
               "any index out of range on empty cache");

    expectTrue(fuse::renderer::ddgi_util::clampProbeIndexForCache(17u, 8u) == 7u,
               "clampProbeIndexForCache clamps to last cache slot");
    expectTrue(fuse::renderer::ddgi_util::clampProbeIndexForCache(3u, 8u) == 3u,
               "clampProbeIndexForCache preserves in-range index");
    expectTrue(fuse::renderer::ddgi_util::clampProbeIndexForCache(99u, 0u) == 0u,
               "clampProbeIndexForCache returns 0 on empty cache");

void testKernelLaunchGuards() {
    using fuse::renderer::gi::ProbeKernelLaunchRejectReason;

    fuse::renderer::gi::DDGIKernelParams params{};
    params.probe_indices_to_update = indices;
    params.probe_update_count = 2u;
    params.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::canLaunchProbeKernels(params),
               "valid kernel params pass launch preflight");

    ProbeKernelLaunchRejectReason reason = ProbeKernelLaunchRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeKernels(params, reason),
               "tryCanLaunchProbeKernels succeeds for valid params");
    expectTrue(reason == ProbeKernelLaunchRejectReason::None, "valid params reject reason is None");

    fuse::renderer::gi::DDGIKernelParams nullIndices = params;
    expectTrue(!fuse::renderer::gi::canLaunchProbeKernels(nullIndices),
               "null probe indices fail launch preflight");
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeKernels(nullIndices, reason),
               "tryCanLaunchProbeKernels returns false for null indices");
    expectTrue(reason == ProbeKernelLaunchRejectReason::NullProbeIndices,
               "null indices reject reason");
    expectTrue(std::string(fuse::renderer::gi::probeKernelLaunchRejectReasonLabel(reason)) ==
                   "null_probe_indices",
               "null indices reject reason label");

    fuse::renderer::gi::DDGIKernelParams zeroCount = params;
    expectTrue(!fuse::renderer::gi::canLaunchProbeKernels(zeroCount),
               "zero probe count fails launch preflight");
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeKernels(zeroCount, reason),
               "tryCanLaunchProbeKernels returns false for zero count");
    expectTrue(reason == ProbeKernelLaunchRejectReason::ZeroProbeCount,
               "zero count reject reason");

    fuse::renderer::gi::DDGIKernelParams zeroRays = params;
    expectTrue(!fuse::renderer::gi::canLaunchProbeKernels(zeroRays),
               "zero rays per probe fails launch preflight");
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeKernels(zeroRays, reason),
               "tryCanLaunchProbeKernels returns false for zero rays");
    expectTrue(reason == ProbeKernelLaunchRejectReason::ZeroRaysPerProbe,
               "zero rays reject reason");

               "launch_probe_trace_kernel rejects invalid params");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(zeroRays, nullptr),
               "launch_probe_blend_kernel rejects invalid params");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "launch_probe_trace_kernel accepts valid params");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(params, nullptr),
               "launch_probe_blend_kernel accepts valid params");
void testProbeCacheIndexGuards() {

        cache[i].irradiance = {static_cast<fuse::f32>(i + 1u), 0.f, 0.f};

    expectTrue(fuse::renderer::ddgi_util::canLookupCacheAtIndex(desc, 8u, 0u),
               "full cache passes lookup preflight");
    expectTrue(fuse::renderer::ddgi_util::canLookupCacheAtIndex(desc, 8u, 999u),
               "lookup preflight ignores index when cache is sized");
    expectTrue(!fuse::renderer::ddgi_util::canLookupCacheAtIndex(desc, 4u, 0u),
               "undersized cache fails lookup preflight");

    fuse::renderer::ProbeCacheLookupRejectReason reason = fuse::renderer::ProbeCacheLookupRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanLookupCacheAtIndex(desc, 8u, 0u, reason),
               "tryCanLookupCacheAtIndex succeeds for sized cache");
    expectTrue(reason == fuse::renderer::ProbeCacheLookupRejectReason::None, "sized cache reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeCacheLookupRejectReasonLabel(reason), "none") == 0,
               "cache reject label is none");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtIndex(desc, 4u, 0u, reason),
               "tryCanLookupCacheAtIndex rejects undersized cache");
    expectTrue(reason == fuse::renderer::ProbeCacheLookupRejectReason::UndersizedCache,
    expectTrue(std::strcmp(fuse::renderer::probeCacheLookupRejectReasonLabel(reason), "undersized_cache") == 0,
               "undersized cache reject label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtIndex(empty, 8u, 0u, reason),
               "empty grid fails cache lookup preflight");
    expectTrue(reason == fuse::renderer::ProbeCacheLookupRejectReason::EmptyGrid,
               "empty grid reports empty_grid reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtIndex(desc, 0u, 0u, reason),
               "zero cache count fails lookup preflight");
    expectTrue(reason == fuse::renderer::ProbeCacheLookupRejectReason::NullCache,
               "zero cache count reports null_cache reason");

    const fuse::renderer::ProbeGridCoord coord{1, 0, 1};
    expectTrue(fuse::renderer::ddgi_util::cacheIndexFromClampedCoord(desc, coord) == 5u,
               "cacheIndexFromClampedCoord maps coord to flat index");

    expectTrue(fuse::renderer::ddgi_util::trySampleCacheAtIndex(desc, cache.data(), 8u, 999u, irradiance),
               "trySampleCacheAtIndex succeeds with OOB probe index via clamp");
    expectNear(irradiance.x, 8.f, 1e-5f, "clamped cache sample reads last probe irradiance");
    expectTrue(!fuse::renderer::ddgi_util::trySampleCacheAtIndex(desc, nullptr, 8u, 0u, irradiance),
               "trySampleCacheAtIndex rejects null cache pointer");
    expectTrue(!fuse::renderer::ddgi_util::trySampleCacheAtIndex(desc, cache.data(), 4u, 0u, irradiance),
               "trySampleCacheAtIndex rejects undersized cache");

    fuse::math::Vec3 coordSample{};
    expectTrue(fuse::renderer::ddgi_util::trySampleCacheAtCoord(desc, cache.data(), 8u, coord, coordSample),
               "trySampleCacheAtCoord succeeds for valid coord");
    expectNear(coordSample.x, 6.f, 1e-5f, "coord cache sample reads probe 5 irradiance");

void testDdgiLaunchGuards() {
    fuse::u32 indices[4] = {0u, 1u, 2u, 3u};

    expectTrue(fuse::renderer::canLaunchDdgiProbeUpdate(desc, indices, 4u),
               "valid launch params pass preflight");
    fuse::renderer::DdgiLaunchRejectReason reason = fuse::renderer::DdgiLaunchRejectReason::None;
    expectTrue(fuse::renderer::preflightDdgiProbeUpdate(desc, indices, 4u, reason),
               "preflightDdgiProbeUpdate succeeds for valid params");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::None, "valid launch reports no reject reason");

    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(empty, indices, 4u, reason),
               "empty grid launch preflight fails");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::EmptyGrid,
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "empty_grid") == 0,
               "empty grid launch reject label");

    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(desc, nullptr, 4u, reason),
               "null indices launch preflight fails");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::NullIndices,
               "null indices reports null_indices launch reason");

    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(desc, indices, 0u, reason),
               "zero probe count launch preflight fails");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count launch reason");

    fuse::renderer::DDGIDesc badRays = desc;
    badRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(badRays, indices, 4u, reason),
               "zero rays_per_probe launch preflight fails");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::InvalidRaysPerProbe,
               "zero rays_per_probe reports invalid_rays_per_probe launch reason");

    params.probe_update_count = 4u;
    expectTrue(fuse::renderer::gi::canLaunchProbeTraceKernel(params),
               "kernel trace preflight passes for valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeBlendKernel(params),
               "kernel blend preflight passes for valid params");

    fuse::renderer::gi::DdgiKernelLaunchRejectReason kernelReason =
        fuse::renderer::gi::DdgiKernelLaunchRejectReason::None;
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(params, kernelReason),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(kernelReason == fuse::renderer::gi::DdgiKernelLaunchRejectReason::None,
               "valid kernel params report no reject reason");

    params.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(params, kernelReason),
               "kernel trace preflight rejects null indices");
    expectTrue(kernelReason == fuse::renderer::gi::DdgiKernelLaunchRejectReason::NullIndices,
               "kernel trace reports null_indices reason");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelLaunchRejectReasonLabel(kernelReason), "null_indices") == 0,
               "kernel null_indices reject label");

    params.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(params, kernelReason),
               "kernel blend preflight rejects zero update count");
    expectTrue(kernelReason == fuse::renderer::gi::DdgiKernelLaunchRejectReason::ZeroUpdateCount,
               "kernel blend reports zero_update_count reason");
    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};

    expectTrue(!fuse::renderer::gi::canLaunchProbeKernels(params),
               "kernel preflight rejects empty params");
    params.probe_indices_to_update = validIndices;
               "kernel preflight accepts non-zero probe list");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "trace kernel launch rejects null indices");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(params, nullptr),
               "blend kernel launch rejects null indices");
               "tryCanLaunchDdgiProbeUpdate accepts valid indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::None,
               "valid launch leaves reject reason None");

               "tryCanLaunchDdgiProbeUpdate rejects null indices");
               "null indices set NullIndices reject reason");
    expectTrue(std::string(fuse::renderer::ddgiLaunchRejectReasonLabel(reason)) == "null_indices",
               "reject reason label for null indices");

               "tryCanLaunchDdgiProbeUpdate rejects zero count");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::ZeroCount,
               "zero count set ZeroCount reject reason");

               "tryCanLaunchDdgiProbeUpdate rejects OOB indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::OutOfRangeIndex,
               "OOB indices set OutOfRangeIndex reject reason");

               "tryCanLaunchDdgiProbeUpdate rejects empty grid");
               "empty grid set EmptyGrid reject reason");

    expectTrue(fuse::renderer::countInvalidLaunchProbeIndices(desc, validIndices, 2u) == 0u,
               "valid indices have zero invalid launch count");
    expectTrue(fuse::renderer::countInvalidLaunchProbeIndices(desc, oobIndices, 2u) == 1u,
               "one OOB index counted as invalid");
    expectTrue(fuse::renderer::countInvalidLaunchProbeIndices(desc, nullptr, 2u) == 0u,
               "null buffer yields zero invalid count");
    expectTrue(fuse::renderer::countInvalidLaunchProbeIndices(empty, validIndices, 2u) == 0u,
               "empty grid yields zero invalid count");
}

    params.hysteresis = 0.97f;

               "trace kernel preflight accepts valid params");
               "blend kernel preflight accepts valid params");

    expectTrue(!fuse::renderer::gi::canLaunchProbeTraceKernel(zeroCount),
               "trace kernel preflight rejects zero update count");

    expectTrue(!fuse::renderer::gi::canLaunchProbeBlendKernel(nullIndices),
               "blend kernel preflight rejects null indices");

    fuse::renderer::gi::DDGIKernelParams badHysteresis = params;
    badHysteresis.hysteresis = 1.5f;
    expectTrue(!fuse::renderer::gi::canLaunchProbeBlendKernel(badHysteresis),
               "blend kernel preflight rejects hysteresis > 1");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(badHysteresis, nullptr),
               "blend kernel launch rejects hysteresis > 1");

    expectTrue(!fuse::renderer::gi::canLaunchProbeTraceKernel(zeroRays),
               "trace kernel preflight rejects zero rays_per_probe");

    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),


    reversed.x0 = 1u;
    reversed.x1 = 0u;

    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;









    fuse::u32 validIndices[2] = {0u, 7u};

    fuse::u32 oobIndices[2] = {0u, 99u};







               "tryCanLaunchDdgiProbeUpdate succeeds for valid indices");
               "valid launch preflight reports none reject reason");

    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "out_of_range_index") == 0,
               "OOB indices report out_of_range_index");

               "tryCanLaunchDdgiProbeUpdate rejects null index buffer");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "null_indices") == 0,
               "null indices report null_indices");

               "tryCanLaunchDdgiProbeUpdate rejects zero probe count");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "zero_count") == 0,
               "zero probe count reports zero_count");

               "empty grid launch reports empty_grid");

void testKernelLaunchPreflights() {
    fuse::renderer::gi::DDGIKernelParams valid{};
    valid.probe_indices_to_update = indices;
    valid.probe_update_count = 2u;
    valid.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::canLaunchDdgiKernelParams(valid),
               "valid kernel params pass preflight");

    fuse::renderer::gi::DdgiKernelLaunchRejectReason reason =
    expectTrue(fuse::renderer::gi::tryCanLaunchDdgiKernelParams(valid, reason),
               "tryCanLaunchDdgiKernelParams succeeds for valid params");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelLaunchRejectReasonLabel(reason), "none") == 0,
               "valid kernel params report none reject reason");

    fuse::renderer::gi::DDGIKernelParams zeroCount = valid;
    expectTrue(!fuse::renderer::gi::canLaunchDdgiKernelParams(zeroCount),
               "zero update count fails kernel preflight");
    expectTrue(!fuse::renderer::gi::tryCanLaunchDdgiKernelParams(zeroCount, reason),
               "tryCanLaunchDdgiKernelParams rejects zero update count");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelLaunchRejectReasonLabel(reason), "zero_update_count") == 0,
               "zero update count reports zero_update_count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = valid;
    expectTrue(!fuse::renderer::gi::tryCanLaunchDdgiKernelParams(nullIndices, reason),
               "tryCanLaunchDdgiKernelParams rejects null probe indices");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelLaunchRejectReasonLabel(reason), "null_probe_indices") == 0,
               "null probe indices report null_probe_indices");

    fuse::renderer::gi::DDGIKernelParams zeroRays = valid;
    expectTrue(!fuse::renderer::gi::tryCanLaunchDdgiKernelParams(zeroRays, reason),
               "tryCanLaunchDdgiKernelParams rejects zero rays per probe");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelLaunchRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero rays per probe reports zero_rays_per_probe");

               "probe trace kernel launch rejects invalid params");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(zeroCount, nullptr),
               "probe blend kernel launch rejects invalid params");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(valid, nullptr),
               "probe trace kernel launch accepts valid params");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(valid, nullptr),
               "probe blend kernel launch accepts valid params");
    fuse::renderer::DdgiLaunchRejectReason launchReason = fuse::renderer::DdgiLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, launchReason),
    expectTrue(launchReason == fuse::renderer::DdgiLaunchRejectReason::None,
               "launch preflight reason None on success");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, oobIndices, 2u, launchReason),
    expectTrue(launchReason == fuse::renderer::DdgiLaunchRejectReason::OutOfRangeIndex,
               "OOB launch reason");
    expectTrue(std::string(fuse::renderer::ddgiLaunchRejectReasonLabel(launchReason)) == "out_of_range_index",
               "OOB launch reason label");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, nullptr, 1u, launchReason),
               "tryCanLaunchDdgiProbeUpdate rejects null buffer");
    expectTrue(launchReason == fuse::renderer::DdgiLaunchRejectReason::NullIndexBuffer,
               "null buffer launch reason");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 0u, launchReason),
    expectTrue(launchReason == fuse::renderer::DdgiLaunchRejectReason::ZeroProbeCount,
               "zero count launch reason");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(empty, validIndices, 2u, launchReason),
    expectTrue(launchReason == fuse::renderer::DdgiLaunchRejectReason::EmptyGrid,
               "empty grid launch reason");

void testKernelLaunchPreflight() {

    fuse::renderer::gi::DdgiKernelRejectReason kernelReason =
        fuse::renderer::gi::DdgiKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::preflightDdgiKernelParams(valid, kernelReason),
               "kernel preflight accepts valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeTraceKernel(valid),
               "canLaunchProbeTraceKernel accepts valid params");
    expectTrue(fuse::renderer::gi::canLaunchProbeBlendKernel(valid),
               "canLaunchProbeBlendKernel accepts valid params");

    expectTrue(!fuse::renderer::gi::preflightDdgiKernelParams(zeroCount, kernelReason),
               "kernel preflight rejects zero update count");
    expectTrue(kernelReason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroUpdateCount,
               "zero update count kernel reason");
    expectTrue(std::string(fuse::renderer::gi::ddgiKernelRejectReasonLabel(kernelReason)) ==
                   "zero_update_count",
               "zero update count kernel reason label");

    expectTrue(!fuse::renderer::gi::preflightDdgiKernelParams(nullIndices, kernelReason),
               "kernel preflight rejects null probe indices");
    expectTrue(kernelReason == fuse::renderer::gi::DdgiKernelRejectReason::NullProbeIndices,
               "null probe indices kernel reason");

    expectTrue(!fuse::renderer::gi::preflightDdgiKernelParams(zeroRays, kernelReason),
               "kernel preflight rejects zero rays per probe");
    expectTrue(kernelReason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroRaysPerProbe,
               "zero rays per probe kernel reason");
               "canLaunchProbeTraceKernel rejects zero rays per probe");

void testProbeSampleCoordRejectReasons() {
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};

    fuse::renderer::ProbeSampleCoordRejectReason reason = fuse::renderer::ProbeSampleCoordRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built, reason),
               "tryBuildProbeSampleCoords succeeds on valid grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordRejectReason::None,
               "valid build reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordRejectReasonLabel(reason), "none") == 0,
               "valid build reject label is none");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(empty, {0.f, 0.f, 0.f}, built, reason),
               "tryBuildProbeSampleCoords rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordRejectReason::EmptyGrid,
               "empty grid reports EmptyGrid reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordRejectReasonLabel(reason), "empty_grid") == 0,
               "empty grid reject label is empty_grid");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 1.f, 1.f};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(badSpacing, {0.5f, 0.5f, 0.5f}, built,
                                                                           reason),
               "tryBuildProbeSampleCoords rejects zero spacing");
               "zero spacing reports EmptyGrid reject reason");

    fuse::renderer::ProbeSampleCoords oobIndices{};
    oobIndices.y0 = 9u;
    oobIndices.y1 = 9u;
    oobIndices.z0 = 9u;
    oobIndices.z1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::isSampleCoordsOutOfRange(desc, oobIndices),
               "isSampleCoordsOutOfRange true before clamp");
    expectTrue(!fuse::renderer::ddgi_util::canSampleAtProbeCoords(desc, oobIndices, 8u),
               "canSampleAtProbeCoords rejects OOB indices");
    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, oobIndices, 8u, reason),
               "tryCanSampleAtProbeCoords rejects OOB indices");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordRejectReason::OutOfBounds,
               "OOB indices report OutOfBounds reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordRejectReasonLabel(reason), "out_of_bounds") == 0,
               "OOB indices reject label is out_of_bounds");

    fuse::renderer::ProbeSampleCoords invalidWeights = built;
    invalidWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, invalidWeights, 8u, reason),
               "tryCanSampleAtProbeCoords rejects invalid weights");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordRejectReason::InvalidWeights,
               "invalid weights report InvalidWeights reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordRejectReasonLabel(reason), "invalid_weights") == 0,
               "invalid weights reject label is invalid_weights");

    expectTrue(fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, built, 8u, reason),
               "tryCanSampleAtProbeCoords succeeds on valid coords and cache");
               "valid sample preflight reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, built, 4u, reason),
               "tryCanSampleAtProbeCoords rejects undersized cache");
               "undersized cache maps to OutOfBounds sample reject reason");

    fuse::renderer::ProbeSampleCoords preserved{};
    preserved.x0 = 9u;
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(empty, preserved),
               "tryClampProbeSampleCoords rejects empty grid without modifying coords");
    expectTrue(preserved.x0 == 9u, "tryClamp leaves coords unchanged on empty grid failure");
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, preserved),
               "tryClampProbeSampleCoords succeeds on valid grid");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, preserved),
               "tryClamp yields valid sample coords");

void testCacheIndexRejectReasons() {

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 0u, 8u, reason),
               "tryIsCacheIndexValid accepts origin probe in full cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache index reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "none") == 0,
               "valid cache index reject label is none");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 8u, 8u, reason),
               "tryIsCacheIndexValid rejects probe index equal to probe count");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbe,
               "probe index equal to probe count reports OutOfRangeProbe");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 3u, 2u, reason),
               "tryIsCacheIndexValid rejects in-range probe with undersized cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "undersized cache reports UndersizedCache");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "undersized_cache") == 0,
               "undersized cache reject label is undersized_cache");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(desc, 99u, 8u, reason),
               "tryIsCacheIndexValid rejects OOB probe index");
               "OOB probe index reports OutOfRangeProbe");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "out_of_range_probe") == 0,
               "OOB probe reject label is out_of_range_probe");

    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(empty, 0u, 8u, reason),
               "tryIsCacheIndexValid rejects empty grid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid reports EmptyGrid cache reject reason");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "empty_grid") == 0,
               "empty grid cache reject label is empty_grid");

void testLaunchRejectReasons() {

    expectTrue(fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "tryCanLaunch accepts in-range probe indices");
               "valid launch reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "none") == 0,
               "valid launch reject label is none");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, oobIndices, 2u, reason),
               "tryCanLaunch rejects OOB probe indices");
               "OOB indices report OutOfRangeIndex");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "out_of_range_index") == 0,
               "OOB launch reject label is out_of_range_index");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, nullptr, 1u, reason),
               "tryCanLaunch rejects null index buffer");
               "null indices report NullIndices");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "null_indices") == 0,
               "null indices reject label is null_indices");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 0u, reason),
               "tryCanLaunch rejects zero probe count");
               "zero count reports ZeroCount");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "zero_count") == 0,
               "zero count reject label is zero_count");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(empty, validIndices, 2u, reason),
               "tryCanLaunch rejects empty grid");
               "empty grid reports EmptyGrid launch reject reason");
               "empty grid launch reject label is empty_grid");

    fuse::u32 indices[2] = {0u, 1u};

    fuse::renderer::gi::DdgiKernelRejectReason reason = fuse::renderer::gi::DdgiKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "tryCanLaunchProbeTraceKernel accepts valid params");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::None,
               "valid trace kernel params report no reject reason");
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeBlendKernel(params, reason),
               "tryCanLaunchProbeBlendKernel accepts valid params");
               "launch_probe_trace_kernel succeeds with valid params");
               "launch_probe_blend_kernel succeeds with valid params");

    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(nullIndices, reason),
               "trace kernel preflight rejects null indices");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::NullIndices,
               "null indices report NullIndices kernel reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "null_indices") == 0,
               "null indices kernel reject label is null_indices");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(nullIndices, nullptr),
               "launch_probe_trace_kernel rejects null indices");

    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(zeroCount, reason),
               "blend kernel preflight rejects zero count");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroCount,
               "zero count reports ZeroCount kernel reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "zero_count") == 0,
               "zero count kernel reject label is zero_count");
               "launch_probe_blend_kernel rejects zero count");

    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(zeroRays, reason),
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::InvalidRaysPerProbe,
               "zero rays_per_probe reports InvalidRaysPerProbe");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "invalid_rays_per_probe") == 0,
               "invalid rays_per_probe kernel reject label is invalid_rays_per_probe");

void testSampleRequestRejectReasons() {
    desc.irradiance_res = 8;

    fuse::renderer::DDGISampleRequest request{};
    request.world_position = {0.5f, 0.5f, 0.5f};
    request.world_normal = {0.f, 1.f, 0.f};

    fuse::renderer::SampleRequestRejectReason reason = fuse::renderer::SampleRequestRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateSampleRequest(desc, request, 8u, reason),
               "valid sample request passes tryValidate");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::None,
               "valid sample request reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::sampleRequestRejectReasonLabel(reason), "none") == 0,
               "none sample-request reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(desc, request, 4u, reason),
               "undersized cache fails sample-request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache sample reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(empty, request, 8u, reason),
               "empty grid fails sample-request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::EmptyGrid,
               "empty grid reports empty_grid sample reason");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateSampleRequest(zeroRes, request, 8u, reason),
               "non-sampleable grid fails sample-request validation");
    expectTrue(reason == fuse::renderer::SampleRequestRejectReason::NotSampleable,
               "zero irradiance_res reports not_sampleable sample reason");
    expectTrue(std::strcmp(fuse::renderer::sampleRequestRejectReasonLabel(reason), "not_sampleable") == 0,
               "not_sampleable sample-request reject reason label");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "valid params do not skip trace kernel preflight");
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "zero update count skips trace kernel preflight");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "null indices skip blend kernel preflight");

    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch trace succeeds for valid params");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "successful tryLaunch trace reports no reject reason");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(nullIndices, nullptr, reason),
               "tryLaunch blend rejects null indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "tryLaunch blend null indices report null_probe_indices reason");

void testProbeScheduleGuards() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 64u, indices, &count),
               "valid schedule preflight succeeds");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "valid schedule is not skipped");

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 64u, &count, reason),
               "trySchedule succeeds for valid inputs");
    expectTrue(count == 64u, "trySchedule writes expected probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "successful schedule reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 64u, indices, 64u, &count, reason),
               "trySchedule rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_probe_count") == 0,
               "zero_probe_count schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 0u, &count, reason),
               "trySchedule rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, nullptr, 64u, &count, reason),
               "trySchedule rejects null output indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputIndices,
               "null output indices report null_output_indices reason");

    fuse::u32 ignoredCount = 99u;
    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 64u, nullptr, reason),
               "trySchedule rejects null output count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputCount,
               "null output count reports null_output_count reason");
    expectTrue(ignoredCount == 99u, "null output count leaves caller count unchanged");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, &count),
               "zero probe count skips schedule preflight");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 0u, indices, &count),
               "zero max indices skips schedule preflight");
    fuse::renderer::gi::DDGIKernelParams nullSurfaces = validParams;
    nullSurfaces.prev_irradiance_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryValidateProbeBlendKernelSurfaces(nullSurfaces, reason),
               "null blend surfaces fail surface preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullBlendSurfaces,
               "null blend surfaces report null_blend_surfaces reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "null_blend_surfaces") == 0,
               "null_blend_surfaces kernel reject reason label");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(nullSurfaces, nullptr),
               "stub blend kernel launch still succeeds without surfaces");
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeBlendKernel(nullSurfaces, reason),
               "null blend surfaces do not affect blend launch preflight");

    fuse::u8 surfaceStorage[3]{};
    fuse::renderer::gi::DDGIKernelParams withSurfaces = validParams;
    withSurfaces.prev_irradiance_surface = &surfaceStorage[0];
    withSurfaces.out_radiance_surface = &surfaceStorage[1];
    withSurfaces.irradiance_atlas_surface = &surfaceStorage[2];
    expectTrue(fuse::renderer::gi::hasProbeBlendKernelSurfaces(withSurfaces),
               "bound blend surfaces pass surface preflight");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch trace rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "tryLaunch trace zero count reports zero_update_count reason");

    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch blend succeeds for valid params");
               "tryLaunch blend rejects null probe indices");
               "tryLaunch blend null indices reports null_probe_indices reason");

void testTrilinearSamplePreflightGuards() {

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearProbeSampleReject none for valid coords");
    const fuse::math::Vec3 centrePos{0.5f, 0.5f, 0.5f};
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeSampleReject(
                   desc, centrePos, cache.data(), 8u) ==
               "classifyTrilinearProbeSampleReject none for valid world position");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, centrePos, cache.data(), 8u),
               "preflightTrilinearProbeIrradiance succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
               "wouldSkipTrilinearProbeIrradiance false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::ProbeTrilinearSampleRejectReason reason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, centrePos, nullptr, 8u, &reason),
               "preflightTrilinearProbeIrradiance rejects null cache");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache trilinear preflight reports null_cache reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, centrePos, nullptr, 8u),
               "wouldSkipTrilinearProbeIrradiance true for null cache");

                   desc, centrePos, cache.data(), 4u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::UndersizedCache,
               "classifyTrilinearProbeSampleReject undersized_cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyTrilinearProbeSampleReject invalid_sample_coords");

    const fuse::math::Vec3 originPos{};
                   empty, originPos, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "classifyTrilinearProbeSampleReject empty_grid");

void testProbeScheduleAtRatePreflightGuards() {

    expectTrue(fuse::renderer::gi::preflightProbeTraceKernelLaunch(kernelParams),
               "preflightProbeTraceKernelLaunch succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernelLaunch(kernelParams),
               "preflightProbeBlendKernelLaunch succeeds for valid params");
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernelLaunch(zeroRays),
               "preflightProbeTraceKernelLaunch rejects zero rays");

    fuse::renderer::gi::DDGIKernelParams populatedPreflight{};
    expectTrue(fuse::renderer::gi::populateAndPreflightDDGIKernelParams(
                   populatedPreflight, desc, validIndices, 2u, 99u),
               "populateAndPreflightDDGIKernelParams succeeds for valid launch");
    expectTrue(populatedPreflight.frame_seed == 99u,
               "populateAndPreflightDDGIKernelParams sets frame_seed");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");
               "successful rate-aware schedule reports no reject reason");


    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(kernelParams),
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(kernelParams),
               "preflightProbeBlendKernel succeeds for valid params");
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroRays),
               "preflightProbeTraceKernel rejects zero rays");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(zeroRays),
               "preflightProbeBlendKernel rejects zero rays");

               "classifyProbeScheduleRejectAtRate none for valid rate");
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 0u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdatesAtRate rejects zero probes_per_frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame schedule reports zero_probes_per_frame reason");

void testPerKernelPreflightGuards() {
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeTraceKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(validParams) ==
               "classifyProbeBlendKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(validParams),
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(validParams),

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(nullIndices) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "classifyProbeBlendKernelReject null_probe_indices");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(nullIndices),
               "preflightProbeBlendKernel rejects null indices");

    fuse::renderer::gi::DDGIKernelParams zeroRays = validParams;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeTraceKernelReject zero_rays_per_probe");
               "preflightProbeTraceKernel rejects zero rays per probe");

    fuse::renderer::gi::ProbeKernelRejectReason kernelReason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroRays, &kernelReason),
               "preflightProbeTraceKernel with reason rejects zero rays");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "preflightProbeTraceKernel reports zero_rays_per_probe reason");
               "tryScheduleProbeUpdatesAtRate zero rate reports zero_probes_per_frame");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSampleAtCoords(
                   desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSampleAtCoords succeeds for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for interior world position");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(
                   desc, preflightCoords, cache.data(), 8u) ==
               "preflightTrilinearProbeSample succeeds for interior sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSampleAtCoords(
               "wouldSkipTrilinearProbeSampleAtCoords false for valid coords");
    const fuse::math::Vec3 interiorWorld{0.5f, 0.5f, 0.5f};
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
                   desc, interiorWorld, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
                   desc, interiorWorld, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache from world position");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, oobIndices, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject invalid_sample_coords");

void testDdgiDeepenFollowUpGuards() {


               "build coords for trilinear deepen follow-up test");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds at valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSampleAtWorldPosition(
               "preflightTrilinearProbeSampleAtWorldPosition succeeds from world position");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, coords, nullptr, 8u, &trilinearReason),
               "preflightTrilinearProbeSample rejects null cache");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject invalid_sample_coords for unordered corners");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, invalid, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for invalid coords");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(empty, coords, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject empty_grid");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSampleAtWorldPosition(
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u, &trilinearReason),
               "preflightTrilinearProbeSampleAtWorldPosition rejects empty grid from world position");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "empty grid world-position trilinear preflight reports empty_grid reason");

    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
                   2u, 2048u, 64u, indices, 64u, &count, scheduleReason),
    expectTrue(indices[0] == 128u, "frame 2 rate-aware schedule starts at probe 128");

    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(
                   2048u, 0u, 64u, indices, &count, &scheduleReason),
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame rate preflight reports zero_probes_per_frame reason");
                   0u, 0u, 64u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate rejects zero probe count");

    fuse::u32 kernelIndices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = kernelIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;

    fuse::renderer::gi::ProbeKernelRejectReason kernelReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
               "preflightProbeTraceKernel zero rays reports zero_rays_per_probe reason");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(zeroRays, &kernelReason),
               "preflightProbeBlendKernel rejects zero rays per probe");
               "preflightProbeBlendKernel zero rays reports zero_rays_per_probe reason");

void testDdgiTrilinearPreflightGuards() {


    const fuse::math::Vec3 centreSample{0.5f, 0.5f, 0.5f};

    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, centreSample, coords),

                   desc, centreSample, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject none for valid world position");
                   desc, centreSample, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearDirectionalProbeSample(
               "preflightTrilinearDirectionalProbeSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearDirectionalProbeSample(
               "wouldSkipTrilinearDirectionalProbeSample false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::tryPreflightTrilinearProbeSample(
                   desc, centreSample, cache.data(), 8u, trilinearReason),
               "tryPreflightTrilinearProbeSample succeeds for valid sample");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "tryPreflightTrilinearProbeSample reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::tryPreflightTrilinearDirectionalProbeSample(
               "tryPreflightTrilinearDirectionalProbeSample succeeds for valid sample");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(!fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, centreSample, nullptr, 8u),
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightTrilinearProbeSample(
                   desc, centreSample, nullptr, 8u, trilinearReason),
               "tryPreflightTrilinearProbeSample rejects null cache");
               "null cache tryPreflight reports null_cache reason");

                   desc, centreSample, cache.data(), 4u) ==
               "classifyProbeTrilinearSampleReject undersized cache");
                   desc, centreSample, cache.data(), 4u),
               "wouldSkipTrilinearProbeSample true for undersized cache");

    const fuse::math::Vec3 emptyWorld{0.f, 0.f, 0.f};
                   empty, emptyWorld, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject empty grid");
                   empty, emptyWorld, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for empty grid");

void testDdgiMandatoryPreflightGuards() {


    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeSchedule(2048u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeSchedule succeeds for valid inputs");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "tryPreflightProbeSchedule reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeScheduleAtRate(
                   2048u, 64u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightProbeScheduleAtRate(
                   2048u, 0u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeScheduleAtRate rejects zero probes_per_frame");
               "tryPreflightProbeScheduleAtRate reports zero_probes_per_frame reason");

    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightCacheIndexLookup(desc, cache.data(), 3u, 8u, cacheReason),
               "tryPreflightCacheIndexLookup succeeds for valid index");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "tryPreflightCacheIndexLookup reports no reject reason");
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightCacheIndexLookup(desc, nullptr, 3u, 8u, cacheReason),
               "tryPreflightCacheIndexLookup rejects null cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "tryPreflightCacheIndexLookup reports null_cache reason");

    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightDdgiProbeUpdate(desc, validIndices, 2u, launchReason),
               "tryPreflightDdgiProbeUpdate succeeds for valid launch");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "tryPreflightDdgiProbeUpdate reports no reject reason");
    expectTrue(!fuse::renderer::tryPreflightDdgiProbeUpdate(desc, nullptr, 1u, launchReason),
               "tryPreflightDdgiProbeUpdate rejects null indices");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::NullIndices,
               "tryPreflightDdgiProbeUpdate reports null_indices reason");

    kernelParams.probe_indices_to_update = validIndices;
    expectTrue(fuse::renderer::gi::tryPreflightProbeKernelLaunch(kernelParams, kernelReason),
               "tryPreflightProbeKernelLaunch succeeds for valid params");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "tryPreflightProbeKernelLaunch reports no reject reason");

    kernelParams.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryPreflightProbeKernelLaunch(kernelParams, kernelReason),
               "tryPreflightProbeKernelLaunch rejects zero rays per probe");
               "tryPreflightProbeKernelLaunch reports zero_rays_per_probe reason");

void testDdgiTrilinearPreflightDeepenGuards() {

               "build coords for trilinear preflight deepen test");

               "preflightTrilinearProbeSample succeeds for valid coords");
               "wouldSkipTrilinearProbeSample false for valid coords");

               "preflightTrilinearProbeIrradiance succeeds for interior sample");
    const fuse::math::Vec3 interiorPos{0.5f, 0.5f, 0.5f};
                   desc, interiorPos, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject none for world position");

    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 8u),
               "preflightTrilinearDirectionalProbeIrradiance succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearDirectionalProbeIrradiance(
               "wouldSkipTrilinearDirectionalProbeIrradiance false for valid sample");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, coords, nullptr, 8u),
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 4u) ==

               "classifyProbeTrilinearSampleReject invalid sample coords");

    const fuse::math::Vec3 originPos{0.f, 0.f, 0.f};
                   empty, originPos, cache.data(), 8u),
               "wouldSkipTrilinearProbeIrradiance true for empty grid");

void testDdgiScheduleAtRatePreflightDeepenGuards() {

                   desc, preflightCoords, cache.data(), 8u),
    const fuse::math::Vec3 interiorSample{0.5f, 0.5f, 0.5f};
                   desc, interiorSample, nullptr, 8u),
                   desc, interiorSample, nullptr, 8u) ==
               "classifyProbeTrilinearSampleReject null cache at world position");


               "tryScheduleProbeUpdatesAtRate succeeds for valid rate");

               "zero probes_per_frame reports zero_probes_per_frame reason");

void testDdgiKernelPreflightDeepenGuards() {



    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(zeroCount) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "classifyProbeTraceKernelReject zero_update_count");
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroCount),
               "preflightProbeTraceKernel rejects zero update count");

void testDdgiTryPreflightDeepenGuards() {


    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleAtRateReject(2048u, 64u, 64u, indices, &count) ==
               "classifyProbeScheduleAtRateReject none for valid rate-aware inputs");

                   0u, 2048u, 64u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid rate-aware inputs");
                   0u, 2048u, 0u, indices, 64u, &count, scheduleReason),
               "tryScheduleProbeUpdatesAtRate zero rate reports zero_probes_per_frame reason");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleAtRateReject(2048u, 0u, 64u, indices, &count) ==
               "classifyProbeScheduleAtRateReject zero_probes_per_frame");

               "tryPreflightCacheIndexLookup succeeds for valid cache");
               "tryPreflightCacheIndexLookup reports no reject reason on success");
               "tryPreflightCacheIndexLookup null cache reports null_cache reason");

               "build coords for trilinear try-preflight test");

                   desc, coords, cache.data(), 8u, trilinearReason),
               "tryPreflightTrilinearProbeSample succeeds on accessible grid");
               "tryPreflightTrilinearProbeSample reports no reject reason on success");

    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeSampleReject(desc, coords, nullptr, 8u) ==
               "classifyTrilinearProbeSampleReject null cache");

    fuse::u32 validLaunchIndices[2] = {0u, 7u};
    expectTrue(fuse::renderer::tryPreflightDdgiProbeUpdate(desc, validLaunchIndices, 2u, launchReason),
               "tryPreflightDdgiProbeUpdate reports no reject reason on success");

    kernelParams.probe_indices_to_update = validLaunchIndices;

    expectTrue(fuse::renderer::gi::tryPreflightProbeTraceKernel(kernelParams, kernelReason),
               "tryPreflightProbeTraceKernel succeeds for valid params");
               "tryPreflightProbeTraceKernel reports no reject reason on success");
    expectTrue(fuse::renderer::gi::tryPreflightProbeBlendKernel(kernelParams, kernelReason),
               "tryPreflightProbeBlendKernel succeeds for valid params");

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "preflightCacheIndexLookup index-only succeeds for valid index");
    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "classifyCacheIndexReject index-only out_of_range_probe_index");

    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(kernelParams) ==
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(kernelParams) ==

    expectTrue(!fuse::renderer::gi::tryPreflightProbeTraceKernel(zeroRays, kernelReason),
               "tryPreflightProbeTraceKernel rejects zero rays per probe");
               "tryPreflightProbeTraceKernel zero rays reports zero_rays_per_probe reason");

void testDdgiThirdLayerPreflightGuards() {



               "tryPreflightProbeScheduleAtRate rejects zero rate");

    expectTrue(!fuse::renderer::ddgi_util::tryPreflightCacheIndexLookup(
                   desc, nullptr, 3u, 8u, cacheReason),

               "build coords for trilinear corner cache-index test");
    expectTrue(fuse::renderer::ddgi_util::areTrilinearCornerCacheIndicesValid(desc, built, 8u),
               "areTrilinearCornerCacheIndicesValid true for full cache");
    expectTrue(!fuse::renderer::ddgi_util::areTrilinearCornerCacheIndicesValid(desc, built, 4u),
               "areTrilinearCornerCacheIndicesValid false for undersized cache");

                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u) ==
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleRejectAtCoords(
                   desc, built, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleRejectAtCoords none for valid coords");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid sample");
    const fuse::math::Vec3 interiorPos{0.5f, 0.5f, 0.5f};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
                   desc, interiorPos, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::tryPreflightTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, trilinearReason),
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),

                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u, trilinearReason),
                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u),
               "tryPreflightTrilinearProbeSample rejects null cache");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache trilinear preflight reports null_cache reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(
                   desc, interiorPos, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");


    expectTrue(fuse::renderer::classifyDdgiHostKernelLaunchReject(desc, kernelParams) ==
                   fuse::renderer::DdgiHostKernelLaunchRejectReason::None,
               "classifyDdgiHostKernelLaunchReject none for valid params");
    expectTrue(fuse::renderer::preflightDdgiHostKernelLaunch(desc, kernelParams),
               "preflightDdgiHostKernelLaunch succeeds for valid params");
    expectTrue(!fuse::renderer::wouldSkipDdgiHostKernelLaunch(desc, kernelParams),
               "wouldSkipDdgiHostKernelLaunch false for valid params");

    fuse::renderer::DdgiHostKernelLaunchRejectReason hostReason =
        fuse::renderer::DdgiHostKernelLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightDdgiHostKernelLaunch(desc, kernelParams, hostReason),
               "tryPreflightDdgiHostKernelLaunch succeeds for valid params");
    expectTrue(hostReason == fuse::renderer::DdgiHostKernelLaunchRejectReason::None,
               "tryPreflightDdgiHostKernelLaunch reports no reject reason on success");
    expectTrue(std::strcmp(fuse::renderer::ddgiHostKernelLaunchRejectReasonLabel(hostReason), "none") == 0,
               "none host kernel launch reject reason label");

    fuse::renderer::gi::DDGIKernelParams oobParams = kernelParams;
    oobParams.probe_indices_to_update = oobIndices;
    expectTrue(fuse::renderer::classifyDdgiHostKernelLaunchReject(desc, oobParams) ==
                   fuse::renderer::DdgiHostKernelLaunchRejectReason::OutOfRangeProbeIndex,
               "classifyDdgiHostKernelLaunchReject out_of_range_probe_index");
    expectTrue(fuse::renderer::wouldSkipDdgiHostKernelLaunch(desc, oobParams),
               "wouldSkipDdgiHostKernelLaunch true for OOB indices");

    expectTrue(fuse::renderer::classifyDdgiHostKernelLaunchReject(desc, zeroRays) ==
                   fuse::renderer::DdgiHostKernelLaunchRejectReason::ZeroRaysPerProbe,
               "classifyDdgiHostKernelLaunchReject zero_rays_per_probe");

               "tryPreflightProbeKernelLaunch reports no reject reason on success");
    expectTrue(!fuse::renderer::gi::tryPreflightProbeKernelLaunch(zeroRays, kernelReason),



               "classifyProbeTrilinearSampleReject none for valid coords and cache");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeSample(desc, built, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for valid coords and cache");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid coords and cache");
                   fuse::renderer::ProbeTrilinearSampleRejectReason::ClampableSampleCoords),
               "clampable_sample_coords trilinear reject reason is not blocking");

                   desc, oobWeights, cache.data(), 8u, trilinearReason),
               "tryPreflightTrilinearProbeSample succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::ClampableSampleCoords,
               "clampable weights report clampable_sample_coords trilinear reason");
    expectTrue(std::strcmp(fuse::renderer::probeTrilinearSampleRejectReasonLabel(trilinearReason),
                           "clampable_sample_coords") == 0,
               "clampable_sample_coords trilinear reject reason label");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, oobWeights, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for clampable weights");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
               "classifyProbeTrilinearSampleReject invalid_sample_coords for hard OOB indices");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, oobIndices, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample true for hard OOB indices");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, built),
               "wouldSkipSampleCoordPreflight false for valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, oobIndices),
               "wouldSkipSampleCoordPreflight true for hard OOB indices");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, oobWeights),
               "wouldSkipSampleCoordPreflight false for clampable weights");

    fuse::renderer::ProbeScheduleRejectReason scheduleReason = fuse::renderer::ProbeScheduleRejectReason::None;
               "tryScheduleProbeUpdatesAtRate reports no reject reason on success");

               "tryScheduleProbeUpdatesAtRate reports zero_probes_per_frame reason");



void testDdgiTrilinearAndGridPreflightGuards() {

    expectTrue(fuse::renderer::ProbeGridLayout::lastProbeIndex(desc) == 7u, "lastProbeIndex for 2x2x2 grid");
    expectTrue(fuse::renderer::ProbeGridLayout::lastProbeIndex(empty) == 0u, "lastProbeIndex zero on empty grid");

    const fuse::renderer::ProbeGridCoord inRange{1, 1, 1};
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, inRange),
               "interior probe coord in range");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordPreflight(desc, inRange),
               "interior probe coord passes preflight");

    const fuse::renderer::ProbeGridCoord oobCoord{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, oobCoord),
               "OOB probe coord flagged out of range");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordPreflight(desc, oobCoord),
               "OOB probe coord fails preflight");

    fuse::renderer::ProbeGridCoord clampedCoord{};
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeGridCoord(desc, oobCoord, clampedCoord),
               "tryClampProbeGridCoord succeeds on non-empty grid");
    expectTrue(clampedCoord.x == 1u && clampedCoord.y == 0u && clampedCoord.z == 0u,
               "tryClampProbeGridCoord clamps OOB coord per axis");
    expectTrue(!fuse::renderer::ProbeGridLayout::tryClampProbeGridCoord(empty, oobCoord, clampedCoord),
               "tryClampProbeGridCoord rejects empty grid");

               "build coords for sample-coord skip test");

    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, hardOob),


                   desc, fuse::math::Vec3{0.5f, 0.5f, 0.5f}, cache.data(), 8u) ==
                   desc, fuse::math::Vec3{0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "preflightTrilinearProbeSample succeeds for world position");
                   desc, fuse::math::Vec3{0.5f, 0.5f, 0.5f}, cache.data(), 4u),
                   desc, fuse::math::Vec3{0.5f, 0.5f, 0.5f}, cache.data(), 4u) ==
               "classifyProbeTrilinearSampleReject undersized_cache");


               "preflightCacheIndexLookup succeeds without cache pointer");
               "classifyCacheIndexReject OOB without cache pointer");


    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(zeroRays) ==
               "classifyProbeBlendKernelReject zero_rays_per_probe");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernelLaunch(zeroRays),
               "preflightProbeBlendKernelLaunch rejects zero rays");


    fuse::renderer::gi::DDGIKernelParams zeroCount = kernelParams;
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(zeroCount),
               "preflightProbeBlendKernel rejects zero update count");

    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkipProbeSampleCoords false for valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, oobIndices),
               "wouldSkipProbeSampleCoords true for hard OOB indices");

               "classifyProbeScheduleAtRateReject none for valid rate");

void testDdgiPreflightDeepenPass2() {

    fuse::renderer::ProbeGridCoord coord{};
    fuse::renderer::ProbeGridSourceRejectReason sourceReason =
        fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryProbeCoordFromIndex(desc, 3u, coord, sourceReason),
               "tryProbeCoordFromIndex succeeds for in-range index");
    expectTrue(coord.x == 1u && coord.y == 1u && coord.z == 0u, "index 3 decodes to (1,1,0)");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid probe-index decode reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(sourceReason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordFromIndex(desc, 3u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeCoordFromIndex none for valid index");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeCoordFromIndex(desc, 3u, &coord),
               "preflightProbeCoordFromIndex succeeds for valid index");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordFromIndex(desc, 3u),
               "wouldSkipProbeCoordFromIndex false for valid index");

    expectTrue(!fuse::renderer::ProbeGridLayout::tryProbeCoordFromIndex(desc, 99u, coord, sourceReason),
               "tryProbeCoordFromIndex rejects OOB index");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::OutOfRangeProbeIndex,
               "OOB probe index reports out_of_range_probe_index source reason");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(sourceReason),
               "out_of_range_probe_index source reason is blocking");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordFromIndex(desc, 99u),
               "wouldSkipProbeCoordFromIndex true for OOB index");

    fuse::u32 encodedIndex = 0u;
    const fuse::renderer::ProbeGridCoord validCoord{1, 0, 1};
    expectTrue(fuse::renderer::ProbeGridLayout::tryProbeIndexFromCoord(desc, validCoord, encodedIndex, sourceReason),
               "tryProbeIndexFromCoord succeeds for valid coord");
    expectTrue(encodedIndex == 5u, "coord (1,0,1) encodes to index 5");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexFromCoord(desc, validCoord) ==
               "classifyProbeIndexFromCoord none for valid coord");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeIndexFromCoord(desc, validCoord, &encodedIndex),
               "preflightProbeIndexFromCoord succeeds for valid coord");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeIndexFromCoord(desc, validCoord),
               "wouldSkipProbeIndexFromCoord false for valid coord");

    const fuse::renderer::ProbeGridCoord invalidCoord{9, 0, 0};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryProbeIndexFromCoord(desc, invalidCoord, encodedIndex, sourceReason),
               "tryProbeIndexFromCoord rejects invalid coord");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::InvalidProbeCoord,
               "invalid coord reports invalid_probe_coord source reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(sourceReason), "invalid_probe_coord") == 0,
               "invalid_probe_coord source reject reason label");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeCoordFromIndex(empty, 0u) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeCoordFromIndex empty_grid on empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeIndexFromCoord(empty, validCoord) ==
               "classifyProbeIndexFromCoord empty_grid on empty grid");

               "build coords for sample preflight deepen pass");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");

    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

               "valid cache tryPreflight reports no reject reason");

    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeTrilinearSample(
                   desc, built, cache.data(), 8u, trilinearReason),
               "tryPreflightProbeTrilinearSample succeeds on accessible grid");
               "valid trilinear preflight reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, built, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid sample");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

               "valid schedule tryPreflight reports no reject reason");
               "zero probes_per_frame tryPreflight reports zero_probes_per_frame reason");

               "valid launch tryPreflight reports no reject reason");
    expectTrue(!fuse::renderer::tryPreflightDdgiProbeUpdate(desc, validIndices, 0u, launchReason),
               "tryPreflightDdgiProbeUpdate rejects zero count");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::ZeroCount,
               "zero count tryPreflight reports zero_count launch reason");

               "valid kernel tryPreflight reports no reject reason");
               "tryPreflightProbeKernelLaunch rejects zero rays_per_probe");
               "zero rays_per_probe tryPreflight reports zero_rays_per_probe reason");

    desc.grid_dims = {4, 2, 3};

    const fuse::renderer::ProbeGridCoord valid{1, 0, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeGridCoordReject(desc, valid) ==
                   fuse::renderer::ProbeGridCoordRejectReason::None,
               "classifyProbeGridCoordReject none for valid coord");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeGridCoord(desc, valid),
               "preflightProbeGridCoord succeeds for valid coord");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordLookup(desc, valid),
               "wouldSkipProbeCoordLookup false for valid coord");

    fuse::u32 indexFromCoord = 0u;
    fuse::renderer::ProbeGridCoordRejectReason coordReason =
        fuse::renderer::ProbeGridCoordRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryProbeIndexFromCoord(desc, valid, indexFromCoord, coordReason),
    expectTrue(indexFromCoord == 17u, "tryProbeIndexFromCoord returns index 17");

    const fuse::renderer::ProbeGridCoord invalid{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeGridCoordReject(desc, invalid) ==
                   fuse::renderer::ProbeGridCoordRejectReason::OutOfRangeCoord,
               "classifyProbeGridCoordReject out_of_range_coord");
    expectTrue(std::strcmp(fuse::renderer::probeGridCoordRejectReasonLabel(
                   fuse::renderer::ProbeGridCoordRejectReason::OutOfRangeCoord),
               "out_of_range_coord") == 0,
               "out_of_range_coord probe-grid reject reason label");
    expectTrue(fuse::renderer::probeGridCoordRejectReasonIsBlocking(
               "out_of_range_coord is blocking");

    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeGridCoordReject(empty, valid) ==
                   fuse::renderer::ProbeGridCoordRejectReason::EmptyGrid,
               "classifyProbeGridCoordReject empty_grid");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeCoordLookup(empty, valid),
               "wouldSkipProbeCoordLookup true on empty grid");


    expectTrue(fuse::renderer::ddgi_util::classifyCacheIndexReject(desc, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classifyCacheIndexReject count-only none for valid index");
               "preflightCacheIndexLookup count-only succeeds");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(24);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 24u) ==
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 24u),
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 24u) ==

    fuse::u32 validIndices[2] = {0u, 23u};
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(desc, kernelParams) ==
               "grid-aware classifyProbeKernelReject none for valid indices");
    expectTrue(fuse::renderer::gi::preflightProbeKernelLaunch(desc, kernelParams),
               "grid-aware preflightProbeKernelLaunch succeeds");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeKernelLaunch(desc, kernelParams),
               "grid-aware wouldSkipProbeKernelLaunch false for valid params");

    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(desc, oobParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex,
               "grid-aware classifyProbeKernelReject out_of_range_probe_index");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(
                   fuse::renderer::gi::ProbeKernelRejectReason::OutOfRangeProbeIndex),
               "out_of_range_probe_index") == 0,
               "out_of_range_probe_index kernel reject reason label");
    expectTrue(fuse::renderer::gi::wouldSkipProbeKernelLaunch(desc, oobParams),
               "grid-aware wouldSkipProbeKernelLaunch true for OOB indices");

    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(empty, kernelParams, kernelReason),
               "grid-aware tryCanLaunchProbeTraceKernel rejects empty grid");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid,
               "empty grid kernel preflight reports empty_grid reason");
                   fuse::renderer::gi::ProbeKernelRejectReason::EmptyGrid),
               "empty_grid") == 0,
               "empty_grid kernel reject reason label");

}

void testDdgiDeepenPassGuards() {
               "tryPreflightProbeKernelLaunch rejects zero rays per probe");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "tryPreflightProbeKernelLaunch reports zero_rays_per_probe reason");

    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeTraceKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(kernelParams) ==
               "classifyProbeBlendKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(kernelParams),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(kernelParams),
               "preflightProbeBlendKernel succeeds for valid params");

    fuse::renderer::ProbeScheduleRejectReason rateScheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdatesAtRate(
                   0u, 2048u, 64u, indices, 64u, &count, rateScheduleReason),
               "tryScheduleProbeUpdatesAtRate succeeds for valid inputs");
    expectTrue(count == 64u, "tryScheduleProbeUpdatesAtRate schedules 64 probes");

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "preflightCacheIndexLookup index-only succeeds for valid index");

void testDdgiProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for sampleable grid");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none grid-source reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(empty) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid),
                           "empty_grid") == 0,
               "empty_grid grid-source reject reason label");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(zeroRes) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res");
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res grid-source reject reason label");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(badSpacing) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
               "invalid_spacing grid-source reject reason is blocking");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen-pass sample-coord wouldSkip test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");
    const fuse::renderer::ProbeGridCoord valid{1, 1, 1};
    const fuse::renderer::ProbeGridCoord invalid{9, 0, 0};
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, invalid),
               "OOB coord flagged out of range");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeCoordOutOfRange(desc, valid),
               "valid coord not out of range");
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(99u, desc),
               "OOB probe index would clamp");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(3u, desc),
               "in-range probe index would not clamp");

    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanLookupAtCoord(desc, valid, 8u, cacheReason),
               "coord lookup preflight succeeds on accessible grid");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "valid coord reports no cache lookup reject reason");
    expectTrue(fuse::renderer::ddgi_util::canLookupAtCoord(desc, valid, 8u),
               "canLookupAtCoord true on accessible grid");

    expectTrue(fuse::renderer::ddgi_util::tryCanLookupAtCoord(desc, invalid, 8u, cacheReason),
               "OOB coord lookup preflight still succeeds with warning");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB coord reports out_of_range_probe_index reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, invalid, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for OOB coord");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, valid, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid coord");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    cache[3u].irradiance = {0.1f, 0.2f, 0.3f};
    const fuse::renderer::ProbeGridCoord coord{1, 1, 0};
    fuse::math::Vec3 irradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtCoord(
                   desc, cache.data(), 8u, coord, irradiance, cacheReason),
               "tryReadIrradianceAtCoord succeeds for valid coord");
    expectNear(irradiance.x, 0.1f, 1e-5f, "coord read returns stored irradiance x");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, cache.data(), 8u, 3u),
               "wouldSkipReadIrradianceAtIndex false for valid index");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipReadIrradianceAtIndex(desc, nullptr, 8u, 3u),
               "wouldSkipReadIrradianceAtIndex true for null cache");

               "build coords for wouldSkip sample-coord test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkipProbeSampleCoords false for valid coords");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, built),
               "wouldSkipSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};
    }

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "wouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "preflightTrilinearProbeIrradiance succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
               "wouldSkipTrilinearProbeIrradiance false for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 8u),
               "wouldSkipTrilinearDirectionalProbeIrradiance false for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "wouldSkipTrilinearProbeSample true for null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u),
               "wouldSkipTrilinearProbeIrradiance true for undersized cache");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid rate");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernelLaunch(kernelParams),
               "preflightProbeTraceKernelLaunch succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernelLaunch(kernelParams),
               "preflightProbeBlendKernelLaunch succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = kernelParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernelLaunch(zeroCount),
               "preflightProbeTraceKernelLaunch rejects zero update count");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernelLaunch(zeroCount),
               "preflightProbeBlendKernelLaunch rejects zero update count");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, oobIndices),
               "wouldSkipProbeSampleCoords true for OOB indices");
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipSampleCoordPreflight(desc, oobIndices),
               "wouldSkipSampleCoordPreflight true for hard OOB indices");

    fuse::renderer::ProbeSampleCoordsRejectReason sampleReason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, oobIndices, sampleReason),
               "wouldSkipProbeSampleCoords with reason true for OOB indices");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeIndices,
               "wouldSkipProbeSampleCoords reports out_of_range_indices reason");

               "wouldSkipTrilinearProbeSample false for accessible coords");
               "wouldSkipTrilinearProbeSample true for null cache at coords");

    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count, &scheduleReason),
               "wouldSkipProbeSchedule false for valid inputs");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "wouldSkipProbeSchedule reports none reason on success");

    fuse::u32 validLaunchIndices[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(!fuse::renderer::wouldSkipDdgiProbeUpdate(desc, validLaunchIndices, 2u, &launchReason),
               "wouldSkipDdgiProbeUpdate false for valid launch");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "wouldSkipDdgiProbeUpdate reports none reason on success");

    fuse::renderer::CacheIndexRejectReason indexReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, 3u, 8u, &indexReason),
               "wouldSkipCacheIndexLookup false for valid index");
    expectTrue(indexReason == fuse::renderer::CacheIndexRejectReason::None,
               "wouldSkipCacheIndexLookup reports none reason on success");
}

void testProbeGridSourceGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeGridSourceRejectReason reason = fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateProbeGridSource(desc, reason),
               "sampleable grid passes probe-grid source validation");
    expectTrue(reason == fuse::renderer::ProbeGridSourceRejectReason::None,
               "valid grid reports no source reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(reason), "none") == 0,
               "none probe-grid source reject reason label");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for sampleable grid");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe-grid source reject reason is not blocking");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(empty) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeGrid(empty),
               "shouldSkipProbeGrid matches wouldSkipProbeGridSource on empty grid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(zeroRes) ==
                   fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes,
               "classifyProbeGridSourceReject zero_irradiance_res");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                               fuse::renderer::ProbeGridSourceRejectReason::ZeroIrradianceRes),
                           "zero_irradiance_res") == 0,
               "zero_irradiance_res source reject reason label");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(badSpacing) ==
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing,
               "classifyProbeGridSourceReject invalid_spacing");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::InvalidSpacing),
               "invalid_spacing source reject reason is blocking");
}

void testProbeTrilinearPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for trilinear preflight test");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "preflightProbeTrilinearSample succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "preflightTrilinearProbeIrradiance succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "wouldSkipTrilinearProbeIrradiance false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeIrradianceReject(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyTrilinearProbeIrradianceReject none for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classifyProbeTrilinearSampleReject null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, coords, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u),
               "wouldSkipTrilinearProbeIrradiance true for null cache");

    fuse::renderer::ProbeSampleCoords invalid = coords;
    invalid.x0 = 1u;
    invalid.x1 = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, invalid, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "classifyProbeTrilinearSampleReject invalid sample coords");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyTrilinearProbeIrradianceReject(
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::EmptyGrid,
               "classifyTrilinearProbeIrradianceReject empty grid");
}

void testProbeSampleCoordWouldSkip() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldSkip sample-coord test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkipProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, oobWeights),
               "wouldSkipProbeSampleCoords false for clampable weights");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, oobIndices),
               "wouldSkipProbeSampleCoords true for hard OOB indices");
}

void testProbeScheduleAtRatePreflight() {
    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 0u, 64u, indices, &count),
               "preflightProbeScheduleAtRate rejects zero probes_per_frame");
}

void testProbeKernelPreflightGuards() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams params{};
    params.probe_indices_to_update = indices;
    params.probe_update_count = 2u;
    params.rays_per_probe = 256u;

    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(params) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeTraceKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(params) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classifyProbeBlendKernelReject none for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(params),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(params),
               "preflightProbeBlendKernel succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroRays = params;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classifyProbeTraceKernelReject zero_rays_per_probe");
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroRays),
               "preflightProbeTraceKernel rejects zero rays per probe");
}

void testDdgiDeepenPassGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::ProbeGridSource source{};
    source.desc = desc;
    source.cache = cache.data();
    source.cache_count = 8u;

    expectTrue(fuse::renderer::ddgi_util::probeCacheMatchesDesc(desc, 8u),
               "probeCacheMatchesDesc true for full cache");
    expectTrue(!fuse::renderer::ddgi_util::probeCacheMatchesDesc(desc, 4u),
               "probeCacheMatchesDesc false for undersized cache");
    expectTrue(fuse::renderer::ddgi_util::isProbeGridSourceAccessible(source),
               "accessible probe grid source passes validation");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipProbeGridSource(source),
               "shouldSkipProbeGridSource false for accessible source");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(source),
               "preflightProbeGridSource succeeds for accessible source");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(source) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for accessible source");
    expectTrue(!fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::None),
               "none probe grid source reject reason is not blocking");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::NullCache),
               "null_cache probe grid source reject reason is blocking");

    fuse::renderer::ProbeGridSource nullCache = source;
    nullCache.cache = nullptr;
    fuse::renderer::ProbeGridSourceRejectReason sourceReason =
        fuse::renderer::ProbeGridSourceRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::tryValidateProbeGridSource(nullCache, sourceReason),
               "tryValidateProbeGridSource rejects null cache");
    expectTrue(sourceReason == fuse::renderer::ProbeGridSourceRejectReason::NullCache,
               "null cache reports null_cache source reason");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(nullCache),
               "wouldSkipProbeGridSource true for null cache");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(sourceReason), "null_cache") == 0,
               "null_cache probe grid source reject reason label");

    fuse::renderer::ProbeGridSource undersized = source;
    undersized.cache_count = 4u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(undersized) ==
                   fuse::renderer::ProbeGridSourceRejectReason::UndersizedCache,
               "classifyProbeGridSourceReject undersized_cache");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for deepen pass sample preflight");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobIndices),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, oobWeights),
               "wouldSkipProbeSampleCoordPreflight false for clampable weights");

    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classifyProbeTrilinearSampleReject none for valid coords");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid coords");
    const fuse::math::Vec3 centrePos{0.5f, 0.5f, 0.5f};
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, centrePos, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid world position");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, cache.data(), 4u),
               "wouldSkipProbeTrilinearSample true for undersized cache");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, trilinearReason),
               "tryPreflightTrilinearProbeIrradiance succeeds for valid sample");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "tryPreflightTrilinearProbeIrradiance reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "preflightTrilinearProbeIrradiance succeeds for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u, trilinearReason),
               "tryPreflightTrilinearProbeIrradiance rejects null cache");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "null cache tryPreflightTrilinear reports null_cache reason");

    expectTrue(fuse::renderer::ddgi_util::tryPreflightTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 8u, trilinearReason),
               "tryPreflightTrilinearDirectionalProbeIrradiance succeeds for valid sample");
    expectTrue(fuse::renderer::ddgi_util::preflightTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 8u),
               "preflightTrilinearDirectionalProbeIrradiance succeeds for valid sample");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeSchedule(2048u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeSchedule succeeds for valid inputs");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "tryPreflightProbeSchedule reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeScheduleAtRate(
                   2048u, 64u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeScheduleAtRate succeeds for valid rate");
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightProbeScheduleAtRate(
                   2048u, 0u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeScheduleAtRate rejects zero probes_per_frame");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes_per_frame reports zero_probes_per_frame reason");

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightDdgiProbeUpdate(desc, validIndices, 2u, launchReason),
               "tryPreflightDdgiProbeUpdate succeeds for valid launch");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "tryPreflightDdgiProbeUpdate reports no reject reason");

    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryPreflightProbeKernelLaunch(kernelParams, kernelReason),
               "tryPreflightProbeKernelLaunch succeeds for valid params");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "tryPreflightProbeKernelLaunch reports no reject reason");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryPreflightProbeKernelLaunch(zeroRays, kernelReason),
               "tryPreflightProbeKernelLaunch rejects zero rays");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "zero rays reports zero_rays_per_probe kernel reason");
}

void testDdgiDeepenFollowUpGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(desc) ==
                   fuse::renderer::ProbeGridSourceRejectReason::None,
               "classifyProbeGridSourceReject none for sampleable grid");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeGridSource(desc),
               "preflightProbeGridSource succeeds for sampleable grid");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc),
               "wouldSkipProbeGridSource false for sampleable grid");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(desc) ==
                   fuse::renderer::ddgi_util::shouldSkipProbeGrid(desc),
               "wouldSkipProbeGridSource matches shouldSkipProbeGrid");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(empty) ==
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid,
               "classifyProbeGridSourceReject empty_grid");
    expectTrue(std::strcmp(fuse::renderer::probeGridSourceRejectReasonLabel(
                   fuse::renderer::ProbeGridSourceRejectReason::EmptyGrid),
               "empty_grid") == 0,
               "empty_grid probe grid source reject reason label");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeGridSource(empty),
               "wouldSkipProbeGridSource true for empty grid");

    fuse::renderer::DDGIDesc zeroSpacing = desc;
    zeroSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(fuse::renderer::ddgi_util::classifyProbeGridSourceReject(zeroSpacing) ==
                   fuse::renderer::ProbeGridSourceRejectReason::NotSampleable,
               "classifyProbeGridSourceReject not_sampleable for zero spacing");
    expectTrue(fuse::renderer::probeGridSourceRejectReasonIsBlocking(
                   fuse::renderer::ProbeGridSourceRejectReason::NotSampleable),
               "not_sampleable probe grid source reject reason is blocking");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for follow-up deepen guards");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, built),
               "wouldSkipProbeSampleCoordPreflight false for valid coords");

    fuse::renderer::ProbeSampleCoords hardOob = built;
    hardOob.x0 = 9u;
    hardOob.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(desc, hardOob),
               "wouldSkipProbeSampleCoordPreflight true for hard OOB indices");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, 1u, 1u, 1u, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for in-range coord");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, cache.data(), 1u, 1u, 1u, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for valid cache pointer");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(desc, 99u, 99u, 99u, 8u),
               "wouldSkipCacheIndexLookupAtCoord false for clampable OOB coord");
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexLookupCoord(desc, 99u, 99u, 99u),
               "wouldClampCacheIndexLookupCoord true for OOB coord");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexLookupCoord(desc, 1u, 1u, 1u),
               "wouldClampCacheIndexLookupCoord false for in-range coord");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookupAtCoord(empty, cache.data(), 0u, 0u, 0u, 8u),
               "wouldSkipCacheIndexLookupAtCoord true for empty grid");

    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndexAtCoord(desc, 99u, 99u, 99u, 8u, cacheReason),
               "tryValidateCacheIndexAtCoord succeeds for clampable OOB coord");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "clampable OOB coord reports out_of_range_probe_index reason");
    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndexAtCoord(desc, nullptr, 1u, 1u, 1u, 8u, cacheReason),
               "tryValidateCacheIndexAtCoord rejects null cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache coord validation reports null_cache reason");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanTrilinearSampleAtProbeCoords(
                   desc, built, cache.data(), 8u, trilinearReason),
               "tryCanTrilinearSampleAtProbeCoords succeeds on accessible grid");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "valid trilinear preflight reports no reject reason");
    expectTrue(fuse::renderer::ddgi_util::canTrilinearSampleAtProbeCoords(desc, built, cache.data(), 8u),
               "canTrilinearSampleAtProbeCoords true for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for valid inputs");

    fuse::renderer::ProbeSampleCoords warnWeights = built;
    warnWeights.tx = 2.f;
    expectTrue(fuse::renderer::ddgi_util::tryCanTrilinearSampleAtProbeCoords(
                   desc, warnWeights, cache.data(), 8u, trilinearReason),
               "tryCanTrilinearSampleAtProbeCoords succeeds for clampable weights");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::ClampableSampleCoords,
               "clampable weights report clampable_sample_coords reason");
    expectTrue(std::strcmp(fuse::renderer::probeTrilinearSampleRejectReasonLabel(trilinearReason),
                           "clampable_sample_coords") == 0,
               "clampable_sample_coords trilinear reject reason label");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "clampable_sample_coords is not blocking for wouldSkip");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, warnWeights, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample false for clampable weights");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, hardOob, cache.data(), 8u),
               "wouldSkipProbeTrilinearSample true for hard OOB sample coords");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeTrilinearSample(desc, built, nullptr, 8u),
               "wouldSkipProbeTrilinearSample true for null cache");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 64u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classifyProbeScheduleRejectAtRate none for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeScheduleAtRate(2048u, 64u, 64u, indices, &count),
               "preflightProbeScheduleAtRate succeeds for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeScheduleRejectAtRate(2048u, 0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "classifyProbeScheduleRejectAtRate zero_probes_per_frame");

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = validIndices;
    kernelParams.probe_update_count = 2u;
    kernelParams.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(kernelParams),
               "preflightProbeTraceKernel succeeds for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(kernelParams),
               "preflightProbeBlendKernel succeeds for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroRays, &kernelReason),
               "preflightProbeTraceKernel rejects zero rays per probe");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "preflightProbeTraceKernel reports zero_rays_per_probe reason");
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(zeroRays, &kernelReason),
               "preflightProbeBlendKernel rejects zero rays per probe");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "preflightProbeBlendKernel reports zero_rays_per_probe reason");
}

void testSampleGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::canSampleProbeGrid(desc), "default grid is sampleable");
    expectTrue(fuse::renderer::ddgi_util::requiredCacheCount(desc) == 8u, "required cache count matches probe count");
    expectTrue(fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 8u), "full cache sized for grid");
    expectTrue(fuse::renderer::ddgi_util::isCacheSizedForGrid(desc,
                                                             fuse::renderer::ddgi_util::requiredCacheCount(desc)),
               "requiredCacheCount satisfies isCacheSizedForGrid");
    expectTrue(!fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 4u), "undersized cache rejected");
    expectTrue(fuse::renderer::ddgi_util::cacheEntriesMissing(desc, 8u) == 0u, "full cache has zero missing entries");
    expectTrue(fuse::renderer::ddgi_util::cacheEntriesMissing(desc, 4u) == 4u, "undersized cache reports shortfall");
    expectTrue(fuse::renderer::ddgi_util::canAccessCacheIndex(desc, 0u, 8u),
               "origin probe index accessible in full cache");
    expectTrue(fuse::renderer::ddgi_util::canAccessCacheIndex(desc, 7u, 8u),
               "last probe index accessible in full cache");
    expectTrue(!fuse::renderer::ddgi_util::canAccessCacheIndex(desc, 7u, 4u),
               "last probe index rejected in undersized cache");
    expectTrue(!fuse::renderer::ddgi_util::canAccessCacheIndex(desc, 99u, 8u),
               "OOB probe index rejected even with full cache");

    fuse::renderer::DDGISampleRequest request{};
    request.world_position = {0.5f, 0.5f, 0.5f};
    request.world_normal = {0.f, 1.f, 0.f};
    expectTrue(fuse::renderer::ddgi_util::isValidSampleRequest(desc, request, 8u),
               "valid sample request with sized cache");
    expectTrue(!fuse::renderer::ddgi_util::isValidSampleRequest(desc, request, 4u),
               "invalid sample request with undersized cache");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::canSampleProbeGrid(empty), "empty grid not sampleable");
    expectTrue(fuse::renderer::ddgi_util::requiredCacheCount(empty) == 0u, "empty grid requires zero cache entries");
    expectTrue(fuse::renderer::ddgi_util::cacheEntriesMissing(empty, 0u) == 0u,
               "empty grid reports zero missing cache entries");
    expectTrue(!fuse::renderer::ddgi_util::isValidSampleRequest(empty, request, 8u),
               "empty grid sample request invalid");

    fuse::renderer::DDGIDesc zeroRes = desc;
    zeroRes.irradiance_res = 0u;
    expectTrue(!fuse::renderer::ddgi_util::canSampleProbeGrid(zeroRes), "zero irradiance_res not sampleable");
    expectTrue(fuse::renderer::ddgi_util::requiredCacheCount(zeroRes) == 0u,
               "zero irradiance_res requires zero cache entries");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(!fuse::renderer::ddgi_util::canSampleProbeGrid(badSpacing), "zero spacing not sampleable");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};
    }
    const fuse::math::Vec3 guarded =
        fuse::renderer::ddgi_util::trilinearProbeIrradiance(desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u);
    expectNear(guarded.x, 0.f, 1e-5f, "undersized cache trilinear returns zero");

    fuse::renderer::DDGISampleRequest emptyNormal{};
    emptyNormal.world_position = {0.5f, 0.5f, 0.5f};
    emptyNormal.world_normal = {0.f, 0.f, 0.f};
    expectTrue(fuse::renderer::ddgi_util::isValidSampleRequest(desc, emptyNormal, 8u),
               "empty normal still valid — resolved at sample time");

    fuse::math::Vec3 tryResult{};
    expectTrue(fuse::renderer::ddgi_util::tryTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, tryResult),
               "tryTrilinearProbeIrradiance succeeds with sized cache");
    expectTrue(tryResult.x > 0.f, "tryTrilinearProbeIrradiance returns non-zero irradiance");

    fuse::math::Vec3 tryRejected{};
    expectTrue(!fuse::renderer::ddgi_util::tryTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u, tryRejected),
               "tryTrilinearProbeIrradiance rejects undersized cache");
    expectNear(tryRejected.x, 0.f, 1e-5f, "tryTrilinearProbeIrradiance clears output on failure");

    fuse::math::Vec3 tryDirectional{};
    expectTrue(fuse::renderer::ddgi_util::tryTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 8u, tryDirectional),
               "tryTrilinearDirectionalProbeIrradiance succeeds with sized cache");
    expectTrue(tryDirectional.x > 0.f, "tryTrilinearDirectionalProbeIrradiance returns non-zero irradiance");
}

void testKernelLaunchGuards() {
    fuse::u32 indices[4] = {0u, 1u, 2u, 3u};
    fuse::renderer::gi::DDGIKernelParams valid{};
    valid.probe_indices_to_update = indices;
    valid.probe_update_count = 4u;
    valid.rays_per_probe = 256u;
    valid.max_ray_distance = 20.f;
    expectTrue(fuse::renderer::gi::canLaunchProbeUpdate(valid), "valid kernel params pass preflight");

    fuse::renderer::gi::DDGIKernelParams nullIndices = valid;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::canLaunchProbeUpdate(nullIndices),
               "null probe index buffer rejected");

    fuse::renderer::gi::DDGIKernelParams zeroCount = valid;
    zeroCount.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::canLaunchProbeUpdate(zeroCount), "zero update count rejected");

    fuse::renderer::gi::DDGIKernelParams zeroRays = valid;
    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::canLaunchProbeUpdate(zeroRays), "zero rays_per_probe rejected");

    fuse::renderer::gi::DDGIKernelParams zeroDistance = valid;
    zeroDistance.max_ray_distance = 0.f;
    expectTrue(!fuse::renderer::gi::canLaunchProbeUpdate(zeroDistance), "zero max_ray_distance rejected");
}

void testProbeSampleAndCacheGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {4, 4, 4};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(desc) == 63u, "max probe index for 4x4x4");
    expectTrue(fuse::renderer::ddgi_util::expectedCacheCount(desc) == 64u, "expected cache count matches probes");
    expectTrue(fuse::renderer::ddgi_util::cacheMatchesGrid(desc, 64u), "exact cache size matches grid");
    expectTrue(!fuse::renderer::ddgi_util::cacheMatchesGrid(desc, 63u), "undersized cache does not match");
    expectTrue(fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 64u), "full cache sized for grid");
    expectTrue(fuse::renderer::ddgi_util::isCacheIndexInRange(0u, 64u), "cache index 0 in range");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexInRange(64u, 64u), "cache index at count is OOB");
    expectTrue(fuse::renderer::ddgi_util::clampCacheIndex(99u, desc, 64u) == 63u,
               "clamp cache index to last probe");
    expectTrue(fuse::renderer::ddgi_util::clampCacheIndex(5u, desc, 4u) == 3u,
               "clamp cache index to undersized buffer");

    fuse::renderer::ProbeSampleCoords interior{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {1.5f, 1.5f, 1.5f}, interior),
               "build interior sample coords");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, interior),
               "interior sample coords valid");
    expectTrue(!fuse::renderer::ProbeGridLayout::isProbeSampleAtGridBorder(desc, interior),
               "interior sample not at grid border");
    expectTrue(fuse::renderer::ProbeGridLayout::hasFullTrilinearNeighbourhood(desc, interior),
               "interior sample has full trilinear neighbourhood");

    fuse::renderer::DDGIDesc borderDesc = desc;
    borderDesc.grid_dims = {3, 3, 3};
    fuse::renderer::ProbeSampleCoords border{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(borderDesc, {0.f, 0.f, 0.f}, border),
               "build border sample coords");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(borderDesc, border),
               "border sample coords valid");
    expectTrue(fuse::renderer::ProbeGridLayout::isProbeSampleAtGridBorder(borderDesc, border),
               "origin sample is at grid border");
    expectTrue(!fuse::renderer::ProbeGridLayout::hasFullTrilinearNeighbourhood(borderDesc, border),
               "border sample lacks full trilinear neighbourhood");

    fuse::renderer::ProbeSampleCoords invalid{};
    invalid.x0 = 5u;
    invalid.x1 = 4u;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalid),
               "inverted x indices invalid");
    invalid = interior;
    invalid.tx = 1.5f;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalid),
               "OOB interpolation weight invalid");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(64);
    cache[21].irradiance = {2.f, 0.f, 0.f};
    const fuse::math::Vec3 sampled =
        fuse::renderer::ddgi_util::sampleIrradianceAtCacheIndex(cache.data(), 64u, 21u);
    expectNear(sampled.x, 2.f, 1e-5f, "guarded cache read returns stored irradiance");
    const fuse::math::Vec3 oobSample =
        fuse::renderer::ddgi_util::sampleIrradianceAtCacheIndex(cache.data(), 64u, 99u);
    expectNear(oobSample.x, 0.f, 1e-5f, "OOB cache read returns zero");
    const fuse::math::Vec3 nullSample =
        fuse::renderer::ddgi_util::sampleIrradianceAtCacheIndex(nullptr, 64u, 0u);
    expectNear(nullSample.x, 0.f, 1e-5f, "null cache read returns zero");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 3, 3};
    expectTrue(fuse::renderer::ProbeGridLayout::maxProbeIndex(empty) == 0u, "empty grid max probe index is 0");
    expectTrue(fuse::renderer::ddgi_util::expectedCacheCount(empty) == 0u, "empty grid expected cache is 0");
    expectTrue(fuse::renderer::ddgi_util::cacheMatchesGrid(empty, 0u), "empty cache matches empty grid");
    expectTrue(fuse::renderer::ddgi_util::clampCacheIndex(5u, empty, 0u) == 0u,
               "clamp cache index on empty grid returns 0");
}

void testProbeSpatialSampleGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    fuse::renderer::ProbeSampleCoordsRejectReason buildReason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   desc, {0.5f, 0.5f, 0.5f}, built, buildReason),
               "tryBuildProbeSampleCoords succeeds on interior sample");
    expectTrue(buildReason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "build reports no reject reason");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(
                   empty, {0.5f, 0.5f, 0.5f}, built, buildReason),
               "tryBuildProbeSampleCoords rejects empty grid");
    expectTrue(buildReason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "build reports empty_grid on empty grid");

    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, built),
               "built coords are in bounds");
    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, oobWeights),
               "OOB weights still in bounds for index check");
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, oobWeights),
               "OOB weights fail validity despite in-bounds indices");

    fuse::renderer::ProbeSpatialSampleRejectReason spatialReason =
        fuse::renderer::ProbeSpatialSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, built, 8u, spatialReason),
               "spatial sample preflight succeeds with valid coords and cache");
    expectTrue(spatialReason == fuse::renderer::ProbeSpatialSampleRejectReason::None,
               "spatial sample reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSpatialSampleRejectReasonLabel(spatialReason), "none") == 0,
               "none spatial sample reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, oobWeights, 8u, spatialReason),
               "spatial sample preflight rejects invalid coords");
    expectTrue(spatialReason == fuse::renderer::ProbeSpatialSampleRejectReason::InvalidSampleCoords,
               "invalid coords report invalid_sample_coords reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, built, 4u, spatialReason),
               "spatial sample preflight rejects undersized cache");
    expectTrue(spatialReason == fuse::renderer::ProbeSpatialSampleRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache spatial reason");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};
    }
    fuse::math::Vec3 sampled{};
    expectTrue(fuse::renderer::ddgi_util::tryTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u, sampled, spatialReason),
               "tryTrilinearProbeIrradiance succeeds with valid inputs");
    expectTrue(sampled.x > 0.f, "tryTrilinearProbeIrradiance returns non-zero irradiance");

    expectTrue(!fuse::renderer::ddgi_util::tryTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u, sampled, spatialReason),
               "tryTrilinearProbeIrradiance rejects null cache");
    expectTrue(spatialReason == fuse::renderer::ProbeSpatialSampleRejectReason::NullCache,
               "null cache reports null_cache spatial reason");
    expectTrue(std::strcmp(fuse::renderer::probeSpatialSampleRejectReasonLabel(spatialReason), "null_cache") == 0,
               "null_cache spatial reject reason label");

    fuse::math::Vec3 directional{};
    expectTrue(!fuse::renderer::ddgi_util::tryTrilinearDirectionalProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, {0.f, 1.f, 0.f}, cache.data(), 4u, directional, spatialReason),
               "tryTrilinearDirectionalProbeIrradiance fails closed on undersized cache");
    expectTrue(spatialReason == fuse::renderer::ProbeSpatialSampleRejectReason::UndersizedCache,
               "directional try sample reports undersized cache");
}

void testCacheLookupAtCoordGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    const fuse::renderer::ProbeGridCoord interior{1, 1, 1};
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryCanLookupCacheAtCoord(desc, interior, 8u, reason),
               "interior coord passes cache lookup preflight");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None, "interior coord reports no reject reason");

    const fuse::renderer::ProbeGridCoord invalid{9, 0, 0};
    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtCoord(desc, invalid, 8u, reason),
               "invalid coord fails cache lookup preflight");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "invalid coord reports out_of_range_probe_index");

    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtCoord(desc, interior, 4u, reason),
               "valid coord fails when cache undersized");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::UndersizedCache,
               "undersized cache reports undersized_cache at coord");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryCanLookupCacheAtCoord(empty, interior, 8u, reason),
               "empty grid fails coord cache lookup");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid reports empty_grid at coord lookup");
}

void testProbeSchedulePreflightGuards() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(8u, 8u, indices, &count, reason),
               "schedule preflight succeeds with valid outputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None, "schedule preflight reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(8u, 8u, nullptr, &count, reason),
               "schedule preflight rejects null index buffer");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutput,
               "null index buffer reports null_output reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(8u, 8u, indices, nullptr, reason),
               "schedule preflight rejects null count pointer");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutput,
               "null count pointer reports null_output reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 8u, indices, &count, reason),
               "schedule preflight rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(8u, 0u, indices, &count, reason),
               "schedule preflight rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");
}

void testZeroRaysPerProbeLaunchGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.rays_per_probe = 0u;

    fuse::u32 validIndices[2] = {0u, 1u};
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, launchReason),
               "launch preflight rejects zero rays_per_probe");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::ZeroRaysPerProbe,
               "zero rays_per_probe reports zero_rays_per_probe launch reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(launchReason), "zero_rays_per_probe") ==
                   0,
               "zero_rays_per_probe launch reject reason label");
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(desc, validIndices, 2u, nullptr),
               "launch rejects zero rays_per_probe desc");

    fuse::renderer::gi::DDGIKernelParams params{};
    params.probe_indices_to_update = validIndices;
    params.probe_update_count = 2u;
    params.rays_per_probe = 0u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, kernelReason),
               "kernel preflight rejects zero rays_per_probe");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "kernel reports zero_rays_per_probe reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(kernelReason), "zero_rays_per_probe") ==
                   0,
               "zero_rays_per_probe kernel reject reason label");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "trace kernel launch rejects zero rays_per_probe");
}

void testDdgiGuardDeepenClassifyAndPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for classify/preflight deepen test");
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classify sample coords returns None for valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(desc, built),
               "preflightProbeSampleCoords passes for valid coords");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, oobWeights) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "classify sample coords returns OutOfRangeWeights");
    fuse::renderer::ProbeSampleCoordsRejectReason sampleReason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(!fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(desc, oobWeights, &sampleReason),
               "preflightProbeSampleCoords rejects OOB weights");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleCoordsRejectReason::OutOfRangeWeights,
               "preflightProbeSampleCoords reports OutOfRangeWeights");

    fuse::renderer::ProbeSampleCoords clampable = oobWeights;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, clampable),
               "wouldClamp true for OOB weights");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "wouldClamp false for valid coords");

    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classify cache index returns None for valid index");
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u),
               "preflightCacheIndexLookup passes for valid index");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "classify cache index returns OutOfRangeProbeIndex");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u),
               "preflightCacheIndexLookup rejects OOB index");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, 99u, 8u),
               "shouldSkipCacheIndexLookup true for OOB index");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, 3u, 8u),
               "shouldSkipCacheIndexLookup false for valid index");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, cache.data(), 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classify cache index with pointer returns None");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::NullCache,
               "classify cache index returns NullCache");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u),
               "shouldSkipCacheIndexLookup true for null cache");
    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, nullptr, 3u, 8u, &cacheReason),
               "preflightCacheIndexLookup rejects null cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "preflightCacheIndexLookup reports NullCache");

    fuse::math::Vec3 irradiance{};
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, cache.data(), 8u, 3u, irradiance, cacheReason),
               "tryRead with reason succeeds for valid index");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "tryRead with reason reports None on success");
    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(desc, nullptr, 8u, 3u, irradiance, cacheReason),
               "tryRead with reason rejects null cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "tryRead with reason reports NullCache");

    expectTrue(fuse::renderer::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classify trilinear sample returns None for valid inputs");
    expectTrue(fuse::renderer::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classify trilinear sample returns NullCache");

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classify schedule returns None for valid inputs");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeSchedule(2048u, 64u, indices, &count),
               "preflightProbeSchedule passes for valid inputs");
    expectTrue(fuse::renderer::classifyProbeScheduleReject(0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "classify schedule returns ZeroProbeCount");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeSchedule(0u, 64u, indices, &count),
               "shouldSkipProbeSchedule true for zero probe count");
    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeSchedule(0u, 64u, indices, &count, &scheduleReason),
               "preflightProbeSchedule rejects zero probe count");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "preflightProbeSchedule reports ZeroProbeCount");

    fuse::u32 validIndices[2] = {0u, 7u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, validIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "classify launch returns None for valid indices");
    expectTrue(fuse::renderer::preflightDdgiProbeUpdate(desc, validIndices, 2u),
               "preflightDdgiProbeUpdate passes for valid indices");
    expectTrue(!fuse::renderer::shouldSkipDdgiProbeUpdate(desc, validIndices, 2u),
               "shouldSkipDdgiProbeUpdate false for valid indices");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, oobIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "classify launch returns OutOfRangeProbeIndex");
    expectTrue(fuse::renderer::shouldSkipDdgiProbeUpdate(desc, oobIndices, 2u),
               "shouldSkipDdgiProbeUpdate true for OOB indices");
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(desc, oobIndices, 2u, &launchReason),
               "preflightDdgiProbeUpdate rejects OOB indices");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "preflightDdgiProbeUpdate reports OutOfRangeProbeIndex");

    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = validIndices;
    validParams.probe_update_count = 2u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(validParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classify kernel returns None for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(validParams),
               "preflightProbeTraceKernel passes for valid params");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(validParams),
               "preflightProbeBlendKernel passes for valid params");
    expectTrue(!fuse::renderer::gi::shouldSkipProbeTraceKernel(validParams),
               "shouldSkipProbeTraceKernel false for valid params");
    expectTrue(!fuse::renderer::gi::shouldSkipProbeBlendKernel(validParams),
               "shouldSkipProbeBlendKernel false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkipProbeTraceKernel false for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroRays = validParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(fuse::renderer::gi::classifyProbeKernelReject(zeroRays) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classify kernel returns ZeroRaysPerProbe");
    expectTrue(fuse::renderer::gi::shouldSkipProbeBlendKernel(zeroRays),
               "shouldSkipProbeBlendKernel true for zero rays");
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(!fuse::renderer::gi::preflightProbeTraceKernel(zeroRays, &kernelReason),
               "preflightProbeTraceKernel rejects zero rays");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "preflightProbeTraceKernel reports ZeroRaysPerProbe");
}

void testPartialCacheTrilinear() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};

    std::vector<fuse::renderer::IrradianceCacheEntry> partialCache(4);
    for (fuse::u32 i = 0; i < 4u; ++i) {
        partialCache[i].irradiance = {1.f, 0.f, 0.f};
    }

    const fuse::math::Vec3 undersized = fuse::renderer::ddgi_util::trilinearProbeIrradiance(
        desc, {0.5f, 0.5f, 0.5f}, partialCache.data(), static_cast<fuse::u32>(partialCache.size()));
    expectTrue(undersized.x >= 0.f, "undersized cache trilinear sample does not assert");
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

void testProbeScheduleGuards() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 64u, indices, &count),
               "canScheduleProbeUpdates accepts valid buffers");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "wouldSkipProbeSchedule false for valid buffers");

    expectTrue(!fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 64u, nullptr, &count),
               "canScheduleProbeUpdates rejects null indices");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, nullptr, &count),
               "wouldSkipProbeSchedule true for null indices");

    expectTrue(!fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 64u, indices, nullptr),
               "canScheduleProbeUpdates rejects null count");
    expectTrue(!fuse::renderer::ddgi_util::canScheduleProbeUpdates(0u, 64u, indices, &count),
               "canScheduleProbeUpdates rejects zero probe count");
    expectTrue(!fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 0u, indices, &count),
               "canScheduleProbeUpdates rejects zero max indices");
}

void testProbeScheduleRejectReasons() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;
    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;

    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(
                   0u, 2048u, 64u, indices, 64u, &count, reason),
               "tryScheduleProbeUpdates succeeds for valid buffers");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "successful schedule reports no reject reason");
    expectTrue(count == 64u, "tryScheduleProbeUpdates writes scheduled count");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, nullptr, &count, reason),
               "tryCanScheduleProbeUpdates rejects null indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputIndices,
               "null indices report null_output_indices schedule reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "null_output_indices") == 0,
               "null_output_indices schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(0u, 64u, indices, &count, reason),
               "tryCanScheduleProbeUpdates rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count schedule reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 0u, indices, &count, reason),
               "tryCanScheduleProbeUpdates rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices schedule reason");
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

    fuse::u32 zeroCount = 99u;
    fuse::renderer::ddgi_util::scheduleProbeUpdates(0u, 0u, 64u, indices, 64u, &zeroCount);
    expectTrue(zeroCount == 0u, "zero probe count schedules zero probes");

    fuse::u32 cappedCount = 0u;
    fuse::renderer::ddgi_util::scheduleProbeUpdates(0u, 2048u, 128u, indices, 64u, &cappedCount);
    expectTrue(cappedCount == 64u, "schedule capped by max_indices");
}

void testProbeScheduleGuards() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(
                   0u, 16u, 4u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates succeeds for valid buffers");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None, "valid schedule reports no reject reason");
    expectTrue(count == 4u, "trySchedule schedules requested probes");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");

    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(16u, 4u, indices, 8u, &count),
               "canScheduleProbeUpdates true for valid buffers");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(16u, 4u, indices, 8u, &count),
               "wouldSkipProbeSchedule false for valid buffers");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, nullptr, 8u, &count, reason),
               "trySchedule rejects null index buffer");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullIndices,
               "null index buffer reports null_indices reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "null_indices") == 0,
               "null_indices schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, indices, 8u, nullptr, reason),
               "trySchedule rejects null count buffer");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullCount,
               "null count buffer reports null_count reason");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(16u, 4u, nullptr, 8u, &count),
               "wouldSkip true for null index buffer");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(16u, 4u, indices, 8u, nullptr),
               "wouldSkip true for null count buffer");

    fuse::u32 noOpCount = 99u;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 64u, indices, 8u, &noOpCount, reason),
               "trySchedule succeeds for zero probe count no-op");
    expectTrue(noOpCount == 0u, "zero probe count yields zero scheduled probes");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "zero probe count no-op reports no reject reason");
}

void testProbeScheduleRejectReasons() {
    fuse::u32 indices[8]{};
    fuse::u32 count = 0u;
    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;

    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates succeeds for valid inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "valid schedule reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "none") == 0,
               "none schedule reject reason label");
    expectTrue(count == 4u, "trySchedule writes scheduled count");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, nullptr, 8u, &count, reason),
               "trySchedule rejects null output indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputIndices,
               "null indices report null_output_indices reason");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "null_output_indices") == 0,
               "null_output_indices schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, indices, 8u, nullptr, reason),
               "trySchedule rejects null output count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutputCount,
               "null count reports null_output_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 4u, indices, 8u, &count, reason),
               "trySchedule rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "zero probe count reports zero_probe_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 16u, 4u, indices, 0u, &count, reason),
               "trySchedule rejects zero max indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroMaxIndices,
               "zero max indices reports zero_max_indices reason");

    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(16u, 8u, indices, &count),
               "canScheduleProbeUpdates true for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(16u, 8u, indices, &count),
               "wouldSkipProbeSchedule false for schedulable inputs");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 8u, indices, &count),
               "wouldSkipProbeSchedule true for zero probe count");
void testProbeSchedulePreflights() {

    expectTrue(fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, 8u, indices, &count, reason),
               "tryCanSchedule succeeds for valid schedule inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None, "valid schedule reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, 8u, nullptr, &count, reason),
               "tryCanSchedule rejects null indices");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullIndices,
               "null indices report null_indices schedule reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, 8u, indices, nullptr, reason),
               "tryCanSchedule rejects null count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullCount,
               "null count reports null_count schedule reason");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(0u, 64u, 8u, indices, &count, reason),
               "tryCanSchedule rejects zero probe count");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, 0u, indices, &count, reason),
               "tryCanSchedule rejects zero max indices");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 0u, 8u, indices, &count, reason),
               "tryCanSchedule rejects zero probes per frame");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbesPerFrame,
               "zero probes per frame reports zero_probes_per_frame reason");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, 8u, indices, &count),
               "wouldSkip true for zero probe count");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, 8u, indices, &count),
               "wouldSkip false for valid schedule");

    count = 99u;
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 8u, &count, reason),
               "trySchedule succeeds for valid inputs");
    expectTrue(count == 8u, "trySchedule caps to max_indices");
    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 8u, indices, 8u, &count, reason),
    expectTrue(count == 0u, "trySchedule zeroes count on rejection");
}

void testWorldToProbeGridCoordPreflight() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {2.f, 2.f, 2.f};
    desc.grid_dims = {3, 3, 3};

    fuse::math::Vec3 gridCoord{};
    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryWorldToProbeGridCoord(desc, {4.f, 2.f, 6.f}, gridCoord, reason),
               "tryWorldToProbeGridCoord succeeds for valid grid");
    expectNear(gridCoord.x, 2.f, 1e-5f, "tryWorldToProbeGridCoord x");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "valid world-to-grid reports no reject reason");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 3, 3};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryWorldToProbeGridCoord(empty, {0.f, 0.f, 0.f}, gridCoord, reason),
               "tryWorldToProbeGridCoord rejects empty grid");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "empty grid reports empty_grid world-to-grid reason");

    fuse::renderer::DDGIDesc badSpacing = desc;
    badSpacing.probe_spacing = {0.f, 2.f, 2.f};
    expectTrue(!fuse::renderer::ProbeGridLayout::tryWorldToProbeGridCoord(badSpacing, {4.f, 2.f, 6.f}, gridCoord, reason),
               "tryWorldToProbeGridCoord rejects zero spacing");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::InvalidSpacing,
               "zero spacing reports invalid_spacing reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "invalid_spacing") == 0,
               "invalid_spacing sample-coord reject reason label");

void testWouldClampProbeSampleCoords() {
void testProbeSchedulePreflightGuards() {

    expectTrue(fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, 8u, &count, reason),

    expectTrue(count == 8u, "trySchedule caps at max_indices");
    expectTrue(indices[0] == 0u, "trySchedule frame 0 starts at probe 0");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(0u, 64u, indices, 8u, &count, reason),
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "zero_probe_count") == 0,
               "zero_probe_count schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, nullptr, 8u, &count, reason),
               "tryCanSchedule rejects null output indices");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, 8u, nullptr, reason),
               "tryCanSchedule rejects null output count");

    expectTrue(!fuse::renderer::ddgi_util::tryCanScheduleProbeUpdates(2048u, 64u, indices, 0u, &count, reason),
               "tryCanSchedule rejects zero max_indices");
               "zero max_indices reports zero_max_indices reason");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipProbeSchedule(0u, 64u, indices, 8u, &count),
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 64u, indices, 8u, &count),
               "wouldSkip false for valid schedule inputs");

    fuse::renderer::ddgi_util::scheduleProbeUpdates(0u, 0u, 64u, indices, 8u, &count);
    expectTrue(count == 0u, "scheduleProbeUpdates still no-ops on zero probe count");

void testProbeSampleCoordPreflight() {
    fuse::u32 count = 99u;

               "tryScheduleProbeUpdates succeeds with valid buffers");
    expectTrue(count == 8u, "trySchedule clamps to max_indices");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, nullptr, 8u, &count, reason),
               "tryScheduleProbeUpdates rejects null output");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::NullOutput,
               "null output reports null_output reason");
    expectTrue(count == 0u, "null output zeroes scheduled count");
    expectTrue(std::strcmp(fuse::renderer::probeScheduleRejectReasonLabel(reason), "null_output") == 0,
               "null_output schedule reject reason label");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 8u, nullptr, reason),
               "tryScheduleProbeUpdates rejects null count pointer");
               "null count reports null_count reason");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 0u, 64u, indices, 8u, &count, reason),
               "tryScheduleProbeUpdates rejects zero probe count");

    expectTrue(!fuse::renderer::ddgi_util::tryScheduleProbeUpdates(0u, 2048u, 64u, indices, 0u, &count, reason),
               "tryScheduleProbeUpdates rejects zero max_indices");

    expectTrue(fuse::renderer::ddgi_util::canScheduleProbeUpdates(2048u, 8u, indices, &count),
               "canScheduleProbeUpdates true for valid buffers");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipProbeSchedule(2048u, 8u, indices, &count),
               "wouldSkipProbeSchedule false for valid buffers");

void testProbeSampleCoordClassifyAndSkip() {
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for wouldClamp test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, built),
               "valid built coords do not need clamp");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, oobIndices),
               "OOB indices would clamp");

    fuse::renderer::ProbeSampleCoords oobWeights = built;
    oobWeights.tx = 2.f;
    expectTrue(fuse::renderer::ProbeGridLayout::wouldClampProbeSampleCoords(desc, oobWeights),
               "OOB weights would clamp");

void testCacheIndexNullCachePreflight() {

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, cache.data(), 8u, 3u, reason),
               "cache-index preflight with valid cache succeeds");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None, "valid cache reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, nullptr, 8u, 3u, reason),
               "cache-index preflight rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");

    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheLookupIndex(99u, desc),
               "wouldClampCacheLookupIndex true for OOB index");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheLookupIndex(7u, desc),
               "wouldClampCacheLookupIndex false for last valid index");

void testTryProbeWorldPosition() {
    desc.grid_origin = {1.f, 2.f, 3.f};

    fuse::math::Vec3 position{};
    expectTrue(fuse::renderer::ddgi_util::tryProbeWorldPosition(desc, 0u, position, reason),
               "tryProbeWorldPosition succeeds for origin index");
    expectNear(position.x, 1.f, 1e-5f, "tryProbeWorldPosition origin x");

    expectTrue(!fuse::renderer::ddgi_util::tryProbeWorldPosition(desc, 99u, position, reason),
               "tryProbeWorldPosition rejects OOB index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "OOB index reports out_of_range_probe_index reason");

    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryProbeWorldPosition(empty, 0u, position, reason),
               "tryProbeWorldPosition rejects empty grid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid reports empty_grid probe-world-position reason");

void testLaunchZeroRaysPerProbe() {
    desc.rays_per_probe = 0u;

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason reason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "tryCanLaunch rejects zero rays per probe");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::ZeroRaysPerProbe,
               "zero rays per probe reports zero_rays_per_probe launch reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe launch reject reason label");
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(desc, validIndices, 2u, nullptr),
               "launch rejects zero rays per probe");

void testProbeKernelSurfacePreflights() {
               "build coords for preflight test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, built),
               "canPreflight succeeds on valid coords");
    expectTrue(fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, built, reason),
               "tryPreflight succeeds on valid coords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "valid preflight reports no reject reason");
               "build coords for classify test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classify returns none for valid coords");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(!fuse::renderer::ProbeGridLayout::canPreflightProbeSampleCoords(desc, reversed),
               "canPreflight rejects unordered corners");
    expectTrue(!fuse::renderer::ProbeGridLayout::tryPreflightProbeSampleCoords(desc, reversed, reason),
               "tryPreflight rejects unordered corners");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "unordered corners report correct preflight reason");
}

void testCacheIndexNullCacheGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};


               "cache-pointer tryValidate succeeds for in-range index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "valid cache-pointer validation reports no reject reason");

               "cache-pointer tryValidate rejects null cache");
    expectTrue(std::strcmp(fuse::renderer::cacheIndexRejectReasonLabel(reason), "null_cache") == 0,
               "null_cache reject reason label");

    expectTrue(fuse::renderer::ddgi_util::isCacheIndexValid(desc, cache.data(), 8u, 3u),
               "isCacheIndexValid with cache pointer succeeds");
    expectTrue(!fuse::renderer::ddgi_util::isCacheIndexValid(desc, nullptr, 8u, 3u),
               "isCacheIndexValid with null cache rejected");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, nullptr, 8u, 3u),
               "wouldSkip true for null cache");
    expectTrue(!fuse::renderer::ddgi_util::wouldSkipCacheIndexLookup(desc, cache.data(), 8u, 3u),
               "wouldSkip false for accessible cache");

    expectTrue(fuse::renderer::ddgi_util::isCacheIndexValid(desc, 3u, 8u),
               "count-only isCacheIndexValid unchanged without cache pointer");

void testProbeKernelTryLaunchGuards() {
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered corners");
    expectTrue(fuse::renderer::ProbeGridLayout::classifyProbeSampleCoordsReject(desc, reversed) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "classify returns unordered_corners");

void testCacheIndexNullCacheGuard() {

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, cache.data(), 3u, 8u, reason),
               "cache-aware tryValidate succeeds for in-range index");
               "cache-aware validation reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(desc, nullptr, 3u, 8u, reason),
               "cache-aware tryValidate rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "null cache reports null_cache reason");
               "null_cache cache-index reject reason label");

void testProbeKernelTryLaunchAndWouldSkip() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;
    validParams.out_radiance_surface = reinterpret_cast<void*>(1u);
    validParams.irradiance_atlas_surface = reinterpret_cast<void*>(2u);
    validParams.depth_atlas_surface = reinterpret_cast<void*>(3u);

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernelWithSurfaces(validParams, reason),
               "trace with surfaces succeeds when radiance surface set");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "valid trace-with-surfaces reports no reject reason");

    fuse::renderer::gi::DDGIKernelParams nullRadiance = validParams;
    nullRadiance.out_radiance_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernelWithSurfaces(nullRadiance, reason),
               "trace with surfaces rejects null radiance surface");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullRadianceSurface,
               "null radiance surface reports null_radiance_surface reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "null_radiance_surface") == 0,
               "null_radiance_surface kernel reject reason label");

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeBlendKernelWithSurfaces(validParams, reason),
               "blend with surfaces succeeds when atlas surfaces set");
               "valid blend-with-surfaces reports no reject reason");

    fuse::renderer::gi::DDGIKernelParams nullAtlas = validParams;
    nullAtlas.irradiance_atlas_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernelWithSurfaces(nullAtlas, reason),
               "blend with surfaces rejects null irradiance atlas");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullIrradianceAtlas,
               "null irradiance atlas reports null_irradiance_atlas reason");

    fuse::renderer::gi::DDGIKernelParams nullDepth = validParams;
    nullDepth.depth_atlas_surface = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernelWithSurfaces(nullDepth, reason),
               "blend with surfaces rejects null depth atlas");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullDepthAtlas,
               "null depth atlas reports null_depth_atlas reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "null_depth_atlas") == 0,
               "null_depth_atlas kernel reject reason label");

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(validParams, reason),
               "base trace preflight still succeeds without surface requirement");

    expectTrue(fuse::renderer::gi::tryLaunch_probe_trace_kernel(validParams, nullptr, reason),
               "tryLaunch trace succeeds with valid params");
               "successful trace tryLaunch reports no reject reason");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip trace false for valid params");

    expectTrue(fuse::renderer::gi::tryLaunch_probe_blend_kernel(validParams, nullptr, reason),
               "tryLaunch blend succeeds with valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip blend false for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;

               "wouldSkipProbeTraceKernel false for valid params");
               "wouldSkipProbeBlendKernel false for valid params");

               "tryLaunch trace succeeds for valid params");
               "successful tryLaunch trace reports no reject reason");
               "tryLaunch blend succeeds for valid params");

    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkipProbeTraceKernel true for zero update count");
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_trace_kernel(zeroCount, nullptr, reason),
               "tryLaunch trace rejects zero update count");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "tryLaunch trace zero count reports zero_update_count reason");
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkip trace true for zero update count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryLaunch_probe_blend_kernel(nullIndices, nullptr, reason),
               "tryLaunch blend rejects null probe indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "tryLaunch blend null indices report null_probe_indices reason");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip blend true for null probe indices");
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

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — DDGI init/update/sample test only\n");
        return;
    }
#else
    std::printf("SKIP: Vulkan backend disabled — DDGI init/update/sample requires device\n");
#endif
    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr || !bootstrap->status().deviceReady) {

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*device);

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        bindless.destroy(*bootstrap->device());
        return;
    }
#endif

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*device, bindless), "resource manager ready for DDGI");

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
    expectTrue(ddgi.cacheEntry(UINT32_MAX).irradiance.x > 0.f, "OOB cacheEntry clamps to valid probe");

    fuse::renderer::DDGIDesc emptyDesc{};
    emptyDesc.grid_dims = {0, 8, 16};
    fuse::renderer::DDGI emptyDdgi;
    expectTrue(!emptyDdgi.init(emptyDesc, resources), "empty grid DDGI init fails");
    expectTrue(!emptyDdgi.isReady(), "empty grid DDGI not ready");

    fuse::u32 emptyIndices[4] = {0u, 1u, 2u, 3u};
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(emptyDesc, emptyIndices, 4u, nullptr),
               "empty grid probe update launch returns false");

    fuse::u32 oobIndices[2] = {0u, 9999u};
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(desc, oobIndices, 2u, nullptr),
               "OOB probe index rejects launch_ddgi_probe_update");

    fuse::renderer::DDGISampleRequest sampleRequest{};
    sampleRequest.world_position = fuse::renderer::ddgi_util::probeWorldPosition(desc, 0u);
    sampleRequest.world_normal = {0.f, 1.f, 0.f};
    const fuse::renderer::DDGISampleResult sample = ddgi.sampleIrradiance(sampleRequest);
    expectTrue(sample.valid, "irradiance sample valid");
    expectTrue(sample.nearest_probe == 0u, "nearest probe at origin cell");

    ddgi.destroy();
    expectTrue(!ddgi.isReady(), "DDGI destroyed");
    resources.destroy();
    bindless.destroy(*device);
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

void testClassifyRejectReasons() {
void testWouldSkipProbeSampleCoords() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "build coords for classify test");
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, built) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "classify sample coords returns none on valid coords");
               "build coords for wouldSkip sample test");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, built),
               "wouldSkip false for valid sample coords");

    fuse::renderer::ProbeSampleCoords reversed = built;
    reversed.x0 = 1u;
    reversed.x1 = 0u;
    expectTrue(fuse::renderer::classifyProbeSampleCoordsReject(desc, reversed) ==
                   fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "classify sample coords returns unordered_corners");

    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::None,
               "classify cache index returns none on valid index");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, 99u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "classify cache index returns out_of_range_probe_index");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, cache.data(), 3u, 8u) ==
               "classify cache index pointer overload returns none");
    expectTrue(fuse::renderer::classifyCacheIndexReject(desc, nullptr, 3u, 8u) ==
                   fuse::renderer::CacheIndexRejectReason::NullCache,
               "classify cache index returns null_cache");

    fuse::u32 indices[2] = {0u, 7u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, indices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "classify launch returns none on valid indices");
    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(fuse::renderer::classifyProbeUpdateLaunchReject(desc, oobIndices, 2u) ==
                   fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "classify launch returns out_of_range_probe_index");

    fuse::u32 count = 0u;
    expectTrue(fuse::renderer::classifyProbeScheduleReject(2048u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::None,
               "classify schedule returns none on valid inputs");
    expectTrue(fuse::renderer::classifyProbeScheduleReject(0u, 64u, indices, &count) ==
                   fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "classify schedule returns zero_probe_count");

    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = indices;
    kernelParams.probe_update_count = 2u;
    expectTrue(fuse::renderer::gi::classifyProbeTraceKernelReject(kernelParams) ==
                   fuse::renderer::gi::ProbeKernelRejectReason::None,
               "classify trace kernel returns none on valid params");
    kernelParams.rays_per_probe = 0u;
                   fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "classify trace kernel returns zero_rays_per_probe");
    kernelParams.rays_per_probe = 256u;
    expectTrue(fuse::renderer::gi::classifyProbeBlendKernelReject(kernelParams) ==
               "classify blend kernel returns none on valid params");
}

void testPreflightHelpers() {
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipProbeSampleCoords(desc, reversed),
               "wouldSkip true for unordered corners");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(fuse::renderer::ProbeGridLayout::wouldSkipBuildProbeSampleCoords(empty),
               "wouldSkipBuild true on empty grid");
    expectTrue(!fuse::renderer::ProbeGridLayout::wouldSkipBuildProbeSampleCoords(desc),
               "wouldSkipBuild false on non-empty grid");

void testPreflightProbeSampleCoords() {

               "build coords for preflight sample test");

    fuse::renderer::ProbeSampleCoordsRejectReason reason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(desc, built, &reason),
               "preflightProbeSampleCoords passes valid coords");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "preflight sample coords reports no reject reason");

    expectTrue(!fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(desc, reversed, &reason),
               "preflightProbeSampleCoords rejects unordered corners");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::UnorderedCorners,
               "preflight sample coords reports unordered_corners reason");
    expectTrue(fuse::renderer::ProbeGridLayout::preflightProbeSampleCoords(desc, built),
               "preflightProbeSampleCoords without reason pointer succeeds");

void testPreflightCacheIndex() {

    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndex(desc, 3u, 8u, &reason),
               "preflightCacheIndex passes in-range index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "preflight cache index reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndex(desc, 99u, 8u, &reason),
               "preflightCacheIndex rejects OOB index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "preflight cache index reports out_of_range_probe_index reason");

    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndex(desc, cache.data(), 3u, 8u, &reason),
               "preflightCacheIndex pointer overload passes valid index");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndex(desc, nullptr, 3u, 8u, &reason),
               "preflightCacheIndex pointer overload rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "preflight cache index pointer overload reports null_cache reason");

void testPreflightProbeSchedule() {
    fuse::u32 indices[64]{};

    fuse::renderer::ProbeScheduleRejectReason reason = fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightProbeSchedule(2048u, 64u, indices, &count, &reason),
               "preflightProbeSchedule passes valid inputs");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::None,
               "preflight schedule reports no reject reason");

    expectTrue(!fuse::renderer::ddgi_util::preflightProbeSchedule(0u, 64u, indices, &count, &reason),
               "preflightProbeSchedule rejects zero probe count");
    expectTrue(reason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "preflight schedule reports zero_probe_count reason");
    expectTrue(fuse::renderer::ddgi_util::preflightProbeSchedule(2048u, 64u, indices, &count),
               "preflightProbeSchedule without reason pointer succeeds");

void testWouldSkipSampleAtProbeCoords() {
void testDdgiShouldSkipTryPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    fuse::renderer::CacheIndexRejectReason cacheReason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 3u, 8u, &cacheReason),
               "preflightCacheIndexLookup passes valid index");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::None,
               "preflight cache index reports none on success");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, 99u, 8u, &cacheReason),
               "preflightCacheIndexLookup rejects OOB index");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "preflight cache index reports out_of_range_probe_index");

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    expectTrue(fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, cache.data(), 3u, 8u),
               "preflight cache pointer overload passes without reason");
    expectTrue(!fuse::renderer::ddgi_util::preflightCacheIndexLookup(desc, nullptr, 3u, 8u, &cacheReason),
               "preflight cache pointer overload rejects null cache");
    expectTrue(cacheReason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "preflight null cache reports null_cache");
    fuse::renderer::ProbeSampleCoords coords{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, coords),
               "build coords for wouldSkip sample-at-coords test");

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipSampleAtProbeCoords(desc, coords, cache.data(), 8u),
               "wouldSkipSampleAtProbeCoords false for accessible grid");

    expectTrue(fuse::renderer::ddgi_util::wouldSkipSampleAtProbeCoords(desc, coords, nullptr, 8u),
               "wouldSkipSampleAtProbeCoords true for null cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipSampleAtProbeCoords(desc, coords, cache.data(), 4u),
               "wouldSkipSampleAtProbeCoords true for undersized cache");
}

void testWouldSkipTrilinearProbeIrradiance() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_origin = {0.f, 0.f, 0.f};
    desc.probe_spacing = {1.f, 1.f, 1.f};
    desc.grid_dims = {2, 2, 2};

    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {1.f, 1.f, 1.f};

    expectTrue(!fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 8u),
               "wouldSkipTrilinear false for full cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearProbeIrradiance(
                   desc, {0.5f, 0.5f, 0.5f}, cache.data(), 4u),
               "wouldSkipTrilinear true for undersized cache");
    expectTrue(fuse::renderer::ddgi_util::wouldSkipTrilinearDirectionalProbeIrradiance(
               "wouldSkipTrilinearDirectional true for undersized cache");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
                   empty, {0.f, 0.f, 0.f}, cache.data(), 8u),
               "wouldSkipTrilinear true for empty grid");

void testPreflightDdgiProbeUpdate() {

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason reason =
        fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::preflightDdgiProbeUpdate(desc, validIndices, 2u, &reason),
               "preflightDdgiProbeUpdate passes valid indices");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "preflight probe update reports no reject reason");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(desc, oobIndices, 2u, &reason),
               "preflightDdgiProbeUpdate rejects OOB indices");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "preflight probe update reports out_of_range_probe_index reason");
    expectTrue(fuse::renderer::preflightDdgiProbeUpdate(desc, validIndices, 2u),
               "preflightDdgiProbeUpdate without reason pointer succeeds");

void testScheduledProbeIndexValidation() {

    fuse::u32 indices[64]{};
    fuse::u32 count = 0u;
    fuse::renderer::ProbeScheduleRejectReason scheduleReason =
        fuse::renderer::ProbeScheduleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::preflightProbeSchedule(2048u, 64u, indices, &count, &scheduleReason),
               "preflightProbeSchedule passes valid inputs");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::None,
               "preflight schedule reports none on success");
    expectTrue(!fuse::renderer::ddgi_util::preflightProbeSchedule(0u, 64u, indices, &count, &scheduleReason),
               "preflightProbeSchedule rejects zero probe count");
    expectTrue(scheduleReason == fuse::renderer::ProbeScheduleRejectReason::ZeroProbeCount,
               "preflight schedule reports zero_probe_count");

    fuse::u32 launchIndices[2] = {0u, 7u};
    fuse::renderer::ProbeUpdateLaunchRejectReason launchReason =
    expectTrue(fuse::renderer::preflightDdgiProbeUpdate(desc, launchIndices, 2u, &launchReason),
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "preflight launch reports none on success");
    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(desc, nullptr, 1u, &launchReason),
               "preflightDdgiProbeUpdate rejects null indices");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::NullIndices,
               "preflight launch reports null_indices");

    fuse::renderer::ProbeSampleCoordsRejectReason sampleReason =
        fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::preflightBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f},
                                                                                &sampleReason),
               "preflightBuildProbeSampleCoords passes non-empty grid");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleCoordsRejectReason::None,
               "preflight sample build reports none on success");
    expectTrue(!fuse::renderer::ProbeGridLayout::preflightBuildProbeSampleCoords(empty, {0.f, 0.f, 0.f},
               "preflightBuildProbeSampleCoords rejects empty grid");
    expectTrue(sampleReason == fuse::renderer::ProbeSampleCoordsRejectReason::EmptyGrid,
               "preflight sample build reports empty_grid");

    fuse::renderer::gi::DDGIKernelParams kernelParams{};
    kernelParams.probe_indices_to_update = launchIndices;
    kernelParams.probe_update_count = 2u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason =
        fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(kernelParams, &kernelReason),
               "preflightProbeTraceKernel passes valid params");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "preflight trace kernel reports none on success");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(kernelParams),
               "preflightProbeBlendKernel passes without reason output");
    kernelParams.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(kernelParams, &kernelReason),
               "preflightProbeBlendKernel rejects zero update count");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroUpdateCount,
               "preflight blend kernel reports zero_update_count");

void testWouldSkipKernelLaunch() {
    expectTrue(fuse::renderer::ddgi_util::tryScheduleProbeUpdates(
                   0u, 8u, 4u, indices, 64u, &count, scheduleReason),
               "schedule probes for validation test");
    expectTrue(count == 4u, "scheduled four probes");

    expectTrue(fuse::renderer::tryValidateScheduledProbeIndices(desc, indices, count, launchReason),
               "scheduled indices pass launch validation");
    expectTrue(!fuse::renderer::wouldSkipScheduledProbeUpdate(desc, indices, count),
               "wouldSkipScheduled false for valid scheduled indices");

    indices[3] = 99u;
    expectTrue(!fuse::renderer::tryValidateScheduledProbeIndices(desc, indices, count, launchReason),
               "corrupted scheduled index fails validation");
    expectTrue(launchReason == fuse::renderer::ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex,
               "corrupted scheduled index reports out_of_range_probe_index reason");
    expectTrue(fuse::renderer::wouldSkipScheduledProbeUpdate(desc, indices, count),
               "wouldSkipScheduled true for corrupted indices");

void testWouldSkipProbeKernels() {
    fuse::u32 indices[2] = {0u, 1u};
    fuse::renderer::gi::DDGIKernelParams validParams{};
    validParams.probe_indices_to_update = indices;
    validParams.probe_update_count = 2u;

    expectTrue(!fuse::renderer::gi::wouldSkipProbeTraceKernel(validParams),
               "wouldSkip trace false for valid params");
    expectTrue(!fuse::renderer::gi::wouldSkipProbeBlendKernel(validParams),
               "wouldSkip blend false for valid params");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(nullIndices),
               "wouldSkip trace true for null indices");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(nullIndices),
               "wouldSkip blend true for null indices");

void testShouldSkipTrilinearSample() {
    desc.irradiance_res = 8;

    expectTrue(!fuse::renderer::ddgi_util::shouldSkipTrilinearProbeSample(
               "shouldSkip trilinear false for accessible grid");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipTrilinearDirectionalProbeSample(
               "shouldSkip directional trilinear false for accessible grid");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipTrilinearProbeSample(
                   desc, {0.5f, 0.5f, 0.5f}, nullptr, 8u),
               "shouldSkip trilinear true for null cache");
               "shouldSkip trilinear true for undersized cache");

               "shouldSkip trilinear true for empty grid");

               "build coords for classify trilinear test");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, cache.data(), 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "classify trilinear sample returns none on valid inputs");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, coords, nullptr, 8u) ==
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache,
               "classify trilinear sample returns null_cache");

void testBuildProbeSampleCoordsIfReady() {

    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoordsIfReady(desc, {0.5f, 0.5f, 0.5f}, coords),
               "buildProbeSampleCoordsIfReady succeeds on interior sample");
    expectTrue(coords.x0 == 0u && coords.x1 == 1u, "IfReady builds expected x corners");

    fuse::renderer::ProbeSampleCoords stale{};
    stale.x0 = 7u;
    expectTrue(!fuse::renderer::ProbeGridLayout::buildProbeSampleCoordsIfReady(empty, {0.f, 0.f, 0.f}, stale),
               "buildProbeSampleCoordsIfReady rejects empty grid");
    expectTrue(stale.x0 == 7u, "IfReady leaves coords unchanged on empty grid failure");

void testWouldClampCacheIndexLookup() {

    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(3u, desc),
               "in-range cache index would not clamp");
    expectTrue(fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(99u, desc),
               "OOB cache index would clamp");
    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(0u, desc),
               "origin cache index would not clamp");

    expectTrue(!fuse::renderer::ddgi_util::wouldClampCacheIndexLookup(99u, empty),
               "empty grid cache index clamp is vacuously false");

void testTryReadIrradianceAtIndexRejectReason() {

    cache[2u].irradiance = {0.5f, 0.25f, 0.125f};

    fuse::math::Vec3 irradiance{};
    fuse::renderer::CacheIndexRejectReason reason = fuse::renderer::CacheIndexRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, cache.data(), 8u, 2u, irradiance, reason),
               "tryRead with reason succeeds for valid index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::None,
               "tryRead with reason reports none on success");
    expectNear(irradiance.x, 0.5f, 1e-5f, "tryRead with reason returns stored irradiance");

    expectTrue(!fuse::renderer::ddgi_util::tryReadIrradianceAtIndex(
                   desc, nullptr, 8u, 2u, irradiance, reason),
               "tryRead with reason rejects null cache");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::NullCache,
               "tryRead with reason reports null_cache");
                   desc, cache.data(), 8u, 99u, irradiance, reason),
               "tryRead with reason rejects OOB index");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::OutOfRangeProbeIndex,
               "tryRead with reason reports out_of_range_probe_index");
               "wouldSkipProbeTraceKernel false for valid params");
               "wouldSkipProbeBlendKernel false for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroCount = validParams;
    zeroCount.probe_update_count = 0u;
    expectTrue(fuse::renderer::gi::wouldSkipProbeTraceKernel(zeroCount),
               "wouldSkipProbeTraceKernel true for zero update count");
    expectTrue(fuse::renderer::gi::wouldSkipProbeBlendKernel(zeroCount),
               "wouldSkipProbeBlendKernel true for zero update count");

void testPreflightProbeKernels() {

    fuse::renderer::gi::ProbeKernelRejectReason reason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(validParams, &reason),
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None,
               "preflight trace kernel reports no reject reason");
    expectTrue(fuse::renderer::gi::preflightProbeBlendKernel(validParams, &reason),
               "preflightProbeBlendKernel passes valid params");

    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(nullIndices, &reason),
               "preflightProbeBlendKernel rejects null indices");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "preflight blend kernel reports null_probe_indices reason");
    expectTrue(fuse::renderer::gi::preflightProbeTraceKernel(validParams),
               "preflightProbeTraceKernel without reason pointer succeeds");

    expectTrue(fuse::renderer::ddgi_util::tryPreflightProbeSchedule(2048u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeSchedule passes for valid inputs");
               "tryPreflightProbeSchedule reports no reject reason on success");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipProbeSchedule(2048u, 64u, indices, &count),
               "shouldSkipProbeSchedule false for valid inputs");
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightProbeSchedule(0u, 64u, indices, &count, scheduleReason),
               "tryPreflightProbeSchedule fails for zero probe count");
               "tryPreflightProbeSchedule reports zero_probe_count reason");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipProbeSchedule(0u, 64u, indices, &count),
               "shouldSkipProbeSchedule true for zero probe count");

    expectTrue(fuse::renderer::ddgi_util::tryPreflightCacheIndexLookup(desc, cache.data(), 3u, 8u, cacheReason),
               "tryPreflightCacheIndexLookup passes for valid index");
               "tryPreflightCacheIndexLookup reports no reject reason on success");
    expectTrue(fuse::renderer::ddgi_util::cacheIndexLookupReady(desc, cache.data(), 3u, 8u),
               "cacheIndexLookupReady true for valid lookup");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, cache.data(), 3u, 8u),
               "shouldSkipCacheIndexLookup false for valid lookup");
    expectTrue(!fuse::renderer::ddgi_util::tryPreflightCacheIndexLookup(desc, nullptr, 3u, 8u, cacheReason),
               "tryPreflightCacheIndexLookup fails for null cache");
               "tryPreflightCacheIndexLookup reports null_cache reason");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipCacheIndexLookup(desc, nullptr, 3u, 8u),
               "shouldSkipCacheIndexLookup true for null cache");

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoordsIfReady(desc, {0.5f, 0.5f, 0.5f}, built),
               "buildProbeSampleCoordsIfReady succeeds for interior sample");
    expectTrue(!fuse::renderer::ProbeGridLayout::shouldSkipProbeSampleCoords(desc, built),
               "shouldSkipProbeSampleCoords false for valid coords");

    fuse::renderer::ProbeSampleCoords oobIndices = built;
    oobIndices.x0 = 9u;
    oobIndices.x1 = 9u;
    expectTrue(fuse::renderer::ProbeGridLayout::shouldSkipProbeSampleCoords(desc, oobIndices),
               "shouldSkipProbeSampleCoords true for OOB indices");

    fuse::renderer::ProbeTrilinearSampleRejectReason trilinearReason =
        fuse::renderer::ProbeTrilinearSampleRejectReason::None;
    expectTrue(fuse::renderer::ddgi_util::tryPreflightTrilinearProbeSample(
                   desc, built, cache.data(), 8u, trilinearReason),
               "tryPreflightTrilinearProbeSample passes for valid sample");
    expectTrue(trilinearReason == fuse::renderer::ProbeTrilinearSampleRejectReason::None,
               "tryPreflightTrilinearProbeSample reports no reject reason on success");
    expectTrue(fuse::renderer::ddgi_util::trilinearProbeSampleReady(desc, built, cache.data(), 8u),
               "trilinearProbeSampleReady true for valid sample");
    expectTrue(!fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(trilinearReason),
               "none trilinear reject reason is not blocking");
    expectTrue(fuse::renderer::probeTrilinearSampleRejectReasonIsBlocking(
                   fuse::renderer::ProbeTrilinearSampleRejectReason::NullCache),
               "null_cache trilinear reject reason is blocking");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, cache.data(), 8u) ==
               "classifyProbeTrilinearSampleReject none for valid sample");
    expectTrue(!fuse::renderer::ddgi_util::shouldSkipTrilinearProbeSample(desc, built, cache.data(), 8u),
               "shouldSkipTrilinearProbeSample false for valid sample");
    expectTrue(fuse::renderer::ddgi_util::shouldSkipTrilinearProbeSample(desc, built, nullptr, 8u),
               "shouldSkipTrilinearProbeSample true for null cache");
    expectTrue(fuse::renderer::ddgi_util::classifyProbeTrilinearSampleReject(desc, built, nullptr, 8u) ==
               "classifyProbeTrilinearSampleReject null_cache");

    fuse::u32 validLaunchIndices[2] = {0u, 7u};
    expectTrue(fuse::renderer::tryPreflightDdgiProbeUpdate(desc, validLaunchIndices, 2u, launchReason),
               "tryPreflightDdgiProbeUpdate passes for valid launch");
               "tryPreflightDdgiProbeUpdate reports no reject reason on success");
    expectTrue(!fuse::renderer::shouldSkipDdgiProbeUpdate(desc, validLaunchIndices, 2u),
               "shouldSkipDdgiProbeUpdate false for valid launch");
    expectTrue(fuse::renderer::shouldSkipDdgiProbeUpdate(desc, nullptr, 1u),
               "shouldSkipDdgiProbeUpdate true for null indices");

    kernelParams.probe_indices_to_update = validLaunchIndices;
    kernelParams.rays_per_probe = 256u;
    fuse::renderer::gi::ProbeKernelRejectReason kernelReason = fuse::renderer::gi::ProbeKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryPreflightProbeKernelLaunch(kernelParams, kernelReason),
               "tryPreflightProbeKernelLaunch passes for valid params");
               "tryPreflightProbeKernelLaunch reports no reject reason on success");
    expectTrue(!fuse::renderer::gi::shouldSkipProbeKernelLaunch(kernelParams),
               "shouldSkipProbeKernelLaunch false for valid params");

    fuse::renderer::gi::DDGIKernelParams zeroRays = kernelParams;
    zeroRays.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryPreflightProbeKernelLaunch(zeroRays, kernelReason),
               "tryPreflightProbeKernelLaunch fails for zero rays");
    expectTrue(kernelReason == fuse::renderer::gi::ProbeKernelRejectReason::ZeroRaysPerProbe,
               "tryPreflightProbeKernelLaunch reports zero_rays_per_probe reason");
    expectTrue(fuse::renderer::gi::shouldSkipProbeKernelLaunch(zeroRays),
               "shouldSkipProbeKernelLaunch true for zero rays");
}

} // namespace

int main() {
    fuse::core::initialize();

    testDdgiDescDefaults();
    testProbeGridMath();
    testProbeGridIndexing();
    testProbeGridWorldCoord();
    testMaxProbeIndexAndTryClamp();
    testProbeIndexClamp();
    testProbeMaxIndexHelpers();
    testProbeSampleCoords();
    testProbeSampleCoordValidation();
    testEmptyProbeGrid();
    testProbeValidityFlags();
    testProbePerAxisClamp();
    testProbeSampleCoordGuards();
    testClampProbeSampleCoords();
    testProbeSampleCoordsGuards();
    testProbeCacheIndexGuards();
    testDdgiLaunchGuards();
    testProbeBorderCounts();
    testBorderProbeIndexGuards();
    testProbeSampleCoordGuards();
    testCacheSizingGuards();
    testProbeSampleSkipClassification();
    testResolveSampleDirectionFromSurface();
    testEmptyDirectionGuards();
    testProbeIndexBoundsHelpers();
    testProbeSampleCoordGuards();
    testProbeSampleCoordPreflight();
    testProbeSampleCoordPreflightHelpers();
    testProbeSampleCoordsInBounds();
    testTryBuildProbeSampleCoords();
    testWouldSkipProbeSampleCoords();
    testWouldSkipTrilinearProbeSample();
    testWouldSkipReadIrradianceAtIndex();
    testWouldSkipProbeKernels();
    testProbeSampleCoordRejectReasons();
    testProbeSampleCoordPreflight();
    testProbeSchedulePreflight();
    testCacheIndexPointerGuards();
    testLaunchZeroRaysPerProbe();
    testProbeKernelResourcePreflight();
    testProbeSampleCoordPreflightGuards();
    testProbeSchedulePreflightGuards();
    testCacheLookupValidation();
    testProbeCacheAccessibilityGuards();
    testCacheIndexLookupGuards();
    testTryCanSampleAtProbeCoordsSoftPreflight();
    testTryReadIrradianceAtIndex();
    testTryCanSampleAtProbeCoords();
    testTryTrilinearProbeIrradiance();
    testTryTrilinearDirectionalProbeIrradiance();
    testWouldSkipDdgiProbeUpdate();
    testWouldSkipProbeSampleCoords();
    testWouldSkipProbeTrilinearSample();
    testWouldSkipKernelLaunch();
    testWouldSkipGuardOverloads();
    testTryLaunchDdgiProbeUpdate();
    testProbeSampleCoordBoundsGuards();
    testProbeIndexBoundsHelpers();
    testTryBuildProbeSampleCoords();
    testCanSampleAtProbeCoords();
    testExtendedCacheLookupPreflight();
    testSampleRequestRejectReasons();
    testProbeSchedulePreflightGuards();
    testClassifyWouldSkipGuards();
    testTryLaunchProbeKernels();
    testProbeSchedulePreflights();
    testCacheIndexNullCache();
    testWouldSkipProbeSampleCoords();
    testProbeKernelWouldSkipAndTryLaunch();
    testCacheIndexNullCacheGuards();
    testProbeKernelTryLaunchGuards();
    testCacheIndexGuards();
    testCacheIndexNullCacheGuard();
    testCacheIndexRejectReasons();
    testProbeScheduleRejectReasons();
    testProbeScheduleAtRateGuards();
    testProbeScheduleCapacityGuards();
    testProbeSampleCoordPreflightGuards();
    testCacheIndexClampAndReadRejectReasons();
    testProbeKernelWouldSkipGuards();
    testClassifyProbeGuardRejectReasons();
    testProbeCoordOutOfRange();
    testWouldClampProbeSampleCoords();
    testPreflightProbeSampleCoords();
    testCanLookupAtProbeIndex();
    testShouldSkipProbeSchedule();
    testClassifyProbeSampleCoordsReject();
    testClassifyCacheIndexReject();
    testValidateScheduledProbeIndices();
    testWouldSkipProbeTrilinearSample();
    testHostLaunchZeroRaysPerProbe();
    testWouldSkipProbeKernels();
    testTryClampProbeSampleCoordsRejectReason();
    testProbeSampleCoordDeepenGuards();
    testCacheIndexDeepenGuards();
    testLaunchProbeUpdateDeepenGuards();
    testKernelLaunchPreflightGuards();
    testProbeSampleCoordsRejectReasons();
    testLaunchRejectReasons();
    testKernelPreflightGuards();
    testProbeSampleCoordsPreflight();
    testCacheIndexPreflight();
    testKernelLaunchPreflight();
    testCacheAccessRejectReasons();
    testSampleRequestRejectReasons();
    testProbeScheduleGuards();
    testProbeSchedulePreflights();
    testWouldSkipProbeSampleCoords();
    testProbeCoordOutOfRangeAndClampCacheLookup();
    testProbeSampleCoordPreflight();
    testTryReadIrradianceAtCoord();
    testTryReadIrradianceAtIndexRejectReason();
    testWouldSkipTrilinearProbeSample();
    testWouldSkipProbeKernels();
    testLaunchProbeUpdateGuards();
    testLaunchProbeUpdateRejectReasons();
    testWouldSkipGuards();
    testTryLaunchProbeKernels();
    testWouldSkipProbeKernels();
    testProbeKernelExtendedPreflight();
    testProbeSchedulePreflight();
    testTryValidateCacheLookup();
    testProbeSampleCoordPreflight();
    testProbeKernelBlendPreflight();
    testProbeKernelResourcePreflights();
    testClassifyAndSkipGuards();
    testClassifyAndWouldClampGuards();
    testClassifyGuardHelpers();
    testDdgiClassifyPreflightGuards();
    testSampleRequestRejectReasons();
    testCacheIndexClassifyAndReady();
    testProbeSampleCoordClassifyAndPreflight();
    testScheduleClassifyAndValidate();
    testKernelWouldSkipAndSurfacePreflight();
    testProbeKernelLaunchGuards();
    testProbeGridSourceRejectReasons();
    testDdgiWouldSkipDeepenGuards();
    testDdgiDeepenGuardPass();
    testDdgiTrilinearSamplePreflightGuards();
    testDdgiScheduleAtRatePreflightGuards();
    testDdgiSampleCoordSkipPreflight();
    testDdgiKernelPerPassPreflightGuards();
    testTrilinearSampleDeepenGuards();
    testProbeScheduleAtRateDeepenGuards();
    testProbeSampleCoordWouldSkip();
    testKernelPerPassPreflight();
    testTrilinearAndSchedulePreflightGuards();
    testDdgiTrilinearDeepenGuards();
    testDdgiTrilinearAndWouldSkipGuards();
    testDdgiTrilinearPreflightDeepenGuards();
    testDdgiKernelGridAwarePreflightGuards();
    testProbeGridPreflightGuards();
    testProbeGridRejectReasons();
    testTrilinearSamplePreflightGuards();
    testProbeGridSourceGuards();
    testTrilinearSamplePreflightDeepen();
    testScheduleAtRateDeepen();
    testKernelDescPreflightDeepen();
    testProbeGridIndexBoundsHelpers();
    testPreflightTrilinearProbeSample();
    testKernelOobProbeIndexGuards();
    testTrilinearWouldSkipGuards();
    testScheduleAtRatePreflightGuards();
    testCacheIndexCountOnlyPreflight();
    testTrilinearAndSchedulePreflightDeepen();
    testGridAwareKernelPreflight();
    testDdgiDeepenPassGuards();
    testDdgiWouldSkipPreflightGuards();
    testScheduledCacheIndexGuards();
    testProbeGridSourcePreflights();
    testProbeTrilinearSamplePreflights();
    testWouldSkipSampleCoordPreflight();
    testProbeScheduleAtRatePreflights();
    testCacheIndexPreflightOverload();
    testDdgiDeepenFollowUpGuards();
    testDdgiKernelUpdatePreflightGuards();
    testProbeScheduleAtRatePreflight();
    testNotSampleableGridBlocking();
    testKernelPreflightGuards();
    testDdgiPreflightDeepenGuards();
    testLaunchProbeUpdateIndexGuard();
    testKernelLaunchGuards();
    testLaunchGuards();
    testKernelLaunchPreflights();
    testProbeSampleCoordPreflightDiagnostics();
    testCacheIndexPreflightDiagnostics();
    testLaunchAndKernelPreflightGuards();
    testProbeScheduleGuards();
    testProbeKernelTryLaunchGuards();
    testProbeKernelWouldSkipAndTryLaunch();
    testKernelWouldSkipAndTryLaunch();
    testDdgiGuardDeepenClassifyAndPreflight();
    testWouldSkipProbeSampleCoords();
    testPreflightProbeSampleCoords();
    testPreflightCacheIndex();
    testPreflightProbeSchedule();
    testWouldSkipSampleAtProbeCoords();
    testWouldSkipTrilinearProbeIrradiance();
    testPreflightDdgiProbeUpdate();
    testScheduledProbeIndexValidation();
    testWouldSkipProbeKernels();
    testPreflightProbeKernels();
    testDdgiThirdPassDeepenGuards();
    testDdgiShouldSkipTryPreflightGuards();
    testDdgiPreflightDeepenPass2();
    testTrilinearSamplePreflightGuards();
    testProbeScheduleAtRatePreflightGuards();
    testPerKernelPreflightGuards();
    testDdgiDeepenFollowUpGuards();
    testDdgiTrilinearPreflightGuards();
    testDdgiMandatoryPreflightGuards();
    testDdgiTrilinearPreflightDeepenGuards();
    testDdgiScheduleAtRatePreflightDeepenGuards();
    testDdgiKernelPreflightDeepenGuards();
    testDdgiTryPreflightDeepenGuards();
    testDdgiThirdLayerPreflightGuards();
    testProbeGridPreflightGuards();
    testProbeTrilinearPreflightGuards();
    testProbeKernelPreflightGuards();
    testDdgiTrilinearAndGridPreflightGuards();
    testProbeGridSourceGuards();
    testCacheIndexPreflightOverload();
    testScheduleAtRatePreflightGuards();
    testKernelPreflightDeepenGuards();
    testDdgiDeepenPassGuards();
    testDdgiProbeGridSourceGuards();
    testProbeSampleCoordWouldSkip();
    testProbeScheduleAtRatePreflight();
    testSampleGuards();
    testProbeSampleAndCacheGuards();
    testKernelLaunchGuards();
    testProbeSpatialSampleGuards();
    testCacheLookupAtCoordGuards();
    testProbeSchedulePreflightGuards();
    testZeroRaysPerProbeLaunchGuards();
    testProbeWorldPositionClamped();
    testProbeAtlasLayout();
    testIrradianceOctahedralEncoding();
    testDirectionalProbeIrradiance();
    testIrradianceLerp();
    testBilinearTileIrradiance();
    testPartialCacheTrilinear();
    testTrilinearProbeIrradiance();
    testProbeScheduling();
    testProbeScheduleRejectReasons();
    testProbeSchedulePreflightGuards();
    testProbeSchedulePreflights();
    testWorldToProbeGridCoordPreflight();
    testWouldClampProbeSampleCoords();
    testCacheIndexNullCachePreflight();
    testTryProbeWorldPosition();
    testLaunchZeroRaysPerProbe();
    testProbeKernelSurfacePreflights();
    testProbeSampleCoordPreflight();
    testCacheIndexNullCacheGuards();
    testProbeKernelTryLaunchGuards();
    testProbeSampleCoordClassifyAndSkip();
    testCacheIndexNullCacheGuard();
    testProbeKernelTryLaunchAndWouldSkip();
    testProbeScheduleGuards();
    testHysteresisBlend();
    testPipelineSlot();
    testDdgiInfo();
    testClassifyRejectReasons();
    testPreflightHelpers();
    testWouldSkipKernelLaunch();
    testShouldSkipTrilinearSample();
    testBuildProbeSampleCoordsIfReady();
    testWouldClampCacheIndexLookup();
    testTryReadIrradianceAtIndexRejectReason();
    testDdgiInitUpdateSample();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_ddgi: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ddgi: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
