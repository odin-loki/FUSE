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
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, unclamped),
               "tryClampProbeSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::areProbeSampleCoordsInBounds(desc, unclamped),
               "tryClampProbeSampleCoords produces in-bounds coords");
}

void testProbeSampleCoordGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built),
               "buildProbeSampleCoords produces coords for interior sample");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, built),
               "built sample coords pass validity guard");

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
}

void testKernelLaunchPreflightGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
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

    fuse::u32 oobIndices[2] = {0u, 99u};
    fuse::renderer::gi::DDGIKernelParams oobParams = params;
    oobParams.probe_indices_to_update = oobIndices;
    expectTrue(!fuse::renderer::gi::tryCanLaunchDdgiKernels(desc, oobParams, reason),
               "combined kernel preflight rejects OOB indices");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::OutOfRangeIndex,
               "OOB indices report out_of_range_index for kernel launch");
    expectTrue(!fuse::renderer::launch_ddgi_probe_update(desc, oobIndices, 2u, nullptr),
               "host launch rejects OOB indices via combined kernel preflight");
}

void testProbeSampleCoordsRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    fuse::renderer::ProbeSampleCoordsRejectReason reason = fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built, reason),
               "tryBuildProbeSampleCoords succeeds on interior sample");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None, "interior build reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "none") == 0,
               "none sample-coords reject label");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
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
}

void testCacheIndexRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

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

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(empty, 0u, 8u, reason),
               "tryIsCacheIndexValid rejects empty grid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid cache index reports EmptyGrid");
}

void testLaunchRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::DdgiLaunchRejectReason reason = fuse::renderer::DdgiLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "tryCanLaunch succeeds for in-range indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::None, "valid launch reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "none") == 0,
               "none launch reject label");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, oobIndices, 2u, reason),
               "tryCanLaunch rejects OOB probe indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::OutOfRangeIndex,
               "OOB probe index reports OutOfRangeIndex");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "out_of_range_index") == 0,
               "out_of_range_index launch reject label");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, nullptr, 1u, reason),
               "tryCanLaunch rejects null index buffer");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::NullIndices,
               "null indices report NullIndices");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 0u, reason),
               "tryCanLaunch rejects zero probe count");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::ZeroCount,
               "zero count reports ZeroCount");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(empty, validIndices, 2u, reason),
               "tryCanLaunch rejects empty grid");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::EmptyGrid,
               "empty grid launch reports EmptyGrid");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid launch reject label");
}

void testKernelPreflightGuards() {
    fuse::renderer::gi::DDGIKernelParams params{};
    fuse::u32 indices[2] = {0u, 1u};
    params.probe_indices_to_update = indices;
    params.probe_update_count = 2u;
    params.rays_per_probe = 256u;

    fuse::renderer::gi::DdgiKernelRejectReason reason = fuse::renderer::gi::DdgiKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "probe trace preflight accepts valid params");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::None,
               "valid trace params report no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "none") == 0,
               "none kernel reject label");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "probe trace launch succeeds with valid params");

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeBlendKernel(params, reason),
               "probe blend preflight accepts valid params");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(params, nullptr),
               "probe blend launch succeeds with valid params");

    params.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "probe trace preflight rejects null indices");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::NullIndices,
               "null indices report NullIndices for trace");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "probe trace launch rejects null indices");

    params.probe_indices_to_update = indices;
    params.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(params, reason),
               "probe blend preflight rejects zero count");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroCount,
               "zero count reports ZeroCount for blend");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(params, nullptr),
               "probe blend launch rejects zero count");

    params.probe_update_count = 1u;
    params.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "probe trace preflight rejects zero rays per probe");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroRaysPerProbe,
               "zero rays per probe reports ZeroRaysPerProbe");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe kernel reject label");
}

void testProbeSampleCoordsRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords built{};
    fuse::renderer::ProbeSampleCoordsRejectReason reason = fuse::renderer::ProbeSampleCoordsRejectReason::None;
    expectTrue(fuse::renderer::ProbeGridLayout::tryBuildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, built, reason),
               "tryBuildProbeSampleCoords succeeds on interior sample");
    expectTrue(reason == fuse::renderer::ProbeSampleCoordsRejectReason::None, "interior build reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeSampleCoordsRejectReasonLabel(reason), "none") == 0,
               "none sample-coords reject label");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
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
}

void testCacheIndexRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

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

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::ddgi_util::tryIsCacheIndexValid(empty, 0u, 8u, reason),
               "tryIsCacheIndexValid rejects empty grid");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid cache index reports EmptyGrid");
}

void testLaunchRejectReasons() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::u32 validIndices[2] = {0u, 7u};
    fuse::renderer::DdgiLaunchRejectReason reason = fuse::renderer::DdgiLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "tryCanLaunch succeeds for in-range indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::None, "valid launch reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "none") == 0,
               "none launch reject label");

    fuse::u32 oobIndices[2] = {0u, 99u};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, oobIndices, 2u, reason),
               "tryCanLaunch rejects OOB probe indices");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::OutOfRangeIndex,
               "OOB probe index reports OutOfRangeIndex");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "out_of_range_index") == 0,
               "out_of_range_index launch reject label");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, nullptr, 1u, reason),
               "tryCanLaunch rejects null index buffer");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::NullIndices,
               "null indices report NullIndices");

    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 0u, reason),
               "tryCanLaunch rejects zero probe count");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::ZeroCount,
               "zero count reports ZeroCount");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::tryCanLaunchDdgiProbeUpdate(empty, validIndices, 2u, reason),
               "tryCanLaunch rejects empty grid");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::EmptyGrid,
               "empty grid launch reports EmptyGrid");
    expectTrue(std::strcmp(fuse::renderer::ddgiLaunchRejectReasonLabel(reason), "empty_grid") == 0,
               "empty_grid launch reject label");
}

void testKernelPreflightGuards() {
    fuse::renderer::gi::DDGIKernelParams params{};
    fuse::u32 indices[2] = {0u, 1u};
    params.probe_indices_to_update = indices;
    params.probe_update_count = 2u;
    params.rays_per_probe = 256u;

    fuse::renderer::gi::DdgiKernelRejectReason reason = fuse::renderer::gi::DdgiKernelRejectReason::None;
    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "probe trace preflight accepts valid params");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::None,
               "valid trace params report no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "none") == 0,
               "none kernel reject label");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "probe trace launch succeeds with valid params");

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeBlendKernel(params, reason),
               "probe blend preflight accepts valid params");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(params, nullptr),
               "probe blend launch succeeds with valid params");

    params.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "probe trace preflight rejects null indices");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::NullIndices,
               "null indices report NullIndices for trace");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(params, nullptr),
               "probe trace launch rejects null indices");

    params.probe_indices_to_update = indices;
    params.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(params, reason),
               "probe blend preflight rejects zero count");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroCount,
               "zero count reports ZeroCount for blend");
    expectTrue(!fuse::renderer::gi::launch_probe_blend_kernel(params, nullptr),
               "probe blend launch rejects zero count");

    params.probe_update_count = 1u;
    params.rays_per_probe = 0u;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(params, reason),
               "probe trace preflight rejects zero rays per probe");
    expectTrue(reason == fuse::renderer::gi::DdgiKernelRejectReason::ZeroRaysPerProbe,
               "zero rays per probe reports ZeroRaysPerProbe");
    expectTrue(std::strcmp(fuse::renderer::gi::ddgiKernelRejectReasonLabel(reason), "zero_rays_per_probe") == 0,
               "zero_rays_per_probe kernel reject label");
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

    expectTrue(!fuse::renderer::ddgi_util::tryValidateCacheIndex(empty, 0u, 8u, reason),
               "empty grid fails cache-index validation");
    expectTrue(reason == fuse::renderer::CacheIndexRejectReason::EmptyGrid,
               "empty grid reports empty_grid cache reject reason");

void testLaunchProbeUpdateRejectReasons() {

    fuse::renderer::ProbeUpdateLaunchRejectReason reason = fuse::renderer::ProbeUpdateLaunchRejectReason::None;
    expectTrue(fuse::renderer::tryCanLaunchDdgiProbeUpdate(desc, validIndices, 2u, reason),
               "tryCanLaunch succeeds for valid indices");
    expectTrue(reason == fuse::renderer::ProbeUpdateLaunchRejectReason::None,
               "valid launch reports no reject reason");
    expectTrue(std::strcmp(fuse::renderer::probeUpdateLaunchRejectReasonLabel(reason), "none") == 0,
               "none launch reject reason label");

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
    expectTrue(!fuse::renderer::ddgi_util::tryCanSampleAtProbeCoords(desc, invalid, cache.data(), 8u, reason),
               "tryCanSampleAtProbeCoords rejects unordered corners");
    expectTrue(reason == fuse::renderer::ProbeTrilinearSampleRejectReason::InvalidSampleCoords,
               "unordered corners report invalid_sample_coords reason");

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

void testProbeKernelLaunchGuards() {

    expectTrue(fuse::renderer::gi::tryCanLaunchProbeTraceKernel(validParams, reason),
               "valid kernel params pass trace preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::None, "valid trace params report no reject reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "none") == 0,
               "none kernel reject reason label");
    expectTrue(fuse::renderer::gi::launch_probe_trace_kernel(validParams, nullptr),
               "trace kernel launch succeeds with valid params");
    expectTrue(fuse::renderer::gi::launch_probe_blend_kernel(validParams, nullptr),
               "blend kernel launch succeeds with valid params");

    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeTraceKernel(zeroCount, reason),
               "zero update count fails trace preflight");
               "zero update count reports zero_update_count reason");
    expectTrue(std::strcmp(fuse::renderer::gi::probeKernelRejectReasonLabel(reason), "zero_update_count") == 0,
               "zero_update_count kernel reject reason label");
    expectTrue(!fuse::renderer::gi::launch_probe_trace_kernel(zeroCount, nullptr),
               "trace kernel launch rejects zero update count");

    fuse::renderer::gi::DDGIKernelParams nullIndices = validParams;
    nullIndices.probe_indices_to_update = nullptr;
    expectTrue(!fuse::renderer::gi::tryCanLaunchProbeBlendKernel(nullIndices, reason),
               "null probe indices fail blend preflight");
    expectTrue(reason == fuse::renderer::gi::ProbeKernelRejectReason::NullProbeIndices,
               "null probe indices report null_probe_indices reason");
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

void testDdgiPreflightDeepenGuards() {


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

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — DDGI init/update/sample test only\n");
        return;
    }
#else
    std::printf("SKIP: Vulkan backend disabled — DDGI init/update/sample requires device\n");
    return;
#endif

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
    testMaxProbeIndexAndTryClamp();
    testProbeIndexClamp();
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
    testProbeSampleCoordsInBounds();
    testTryBuildProbeSampleCoords();
    testProbeSampleCoordRejectReasons();
    testProbeCacheAccessibilityGuards();
    testTryReadIrradianceAtIndex();
    testTryCanSampleAtProbeCoords();
    testTryTrilinearProbeIrradiance();
    testTryTrilinearDirectionalProbeIrradiance();
    testWouldSkipDdgiProbeUpdate();
    testTryLaunchDdgiProbeUpdate();
    testProbeSampleCoordBoundsGuards();
    testCacheIndexGuards();
    testCacheIndexRejectReasons();
    testProbeScheduleRejectReasons();
    testProbeScheduleAtRateGuards();
    testTryClampProbeSampleCoordsRejectReason();
    testProbeSampleCoordDeepenGuards();
    testCacheIndexDeepenGuards();
    testLaunchProbeUpdateDeepenGuards();
    testKernelLaunchPreflightGuards();
    testProbeSampleCoordsRejectReasons();
    testLaunchRejectReasons();
    testKernelPreflightGuards();
    testLaunchProbeUpdateGuards();
    testLaunchProbeUpdateRejectReasons();
    testTryLaunchProbeKernels();
    testWouldSkipProbeKernels();
    testProbeKernelLaunchGuards();
    testProbeGridSourceRejectReasons();
    testDdgiWouldSkipDeepenGuards();
    testDdgiPreflightDeepenGuards();
    testLaunchProbeUpdateIndexGuard();
    testKernelLaunchGuards();
    testLaunchGuards();
    testSampleGuards();
    testProbeSampleAndCacheGuards();
    testKernelLaunchGuards();
    testProbeWorldPositionClamped();
    testProbeAtlasLayout();
    testIrradianceOctahedralEncoding();
    testDirectionalProbeIrradiance();
    testIrradianceLerp();
    testBilinearTileIrradiance();
    testPartialCacheTrilinear();
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
