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

void testProbeSampleCoordsGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};

    fuse::renderer::ProbeSampleCoords valid{};
    expectTrue(fuse::renderer::ProbeGridLayout::buildProbeSampleCoords(desc, {0.5f, 0.5f, 0.5f}, valid),
               "buildProbeSampleCoords succeeds for guard validation");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, valid),
               "built sample coords pass validity guard");

    fuse::renderer::ProbeSampleCoords invalid{};
    invalid.x0 = 9u;
    invalid.y0 = 9u;
    invalid.z0 = 9u;
    invalid.x1 = 9u;
    invalid.y1 = 9u;
    invalid.z1 = 9u;
    invalid.tx = 2.f;
    invalid.ty = -1.f;
    invalid.tz = 0.5f;
    expectTrue(!fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalid),
               "OOB sample coords fail validity guard");
    expectTrue(fuse::renderer::ProbeGridLayout::tryClampProbeSampleCoords(desc, invalid),
               "tryClampProbeSampleCoords succeeds on non-empty grid");
    expectTrue(fuse::renderer::ProbeGridLayout::isValidProbeSampleCoords(desc, invalid),
               "clamped sample coords pass validity guard");
    expectTrue(invalid.x0 == 1u && invalid.x1 == 1u, "tryClamp clamps x indices");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    fuse::renderer::ProbeSampleCoords emptyCoords{};
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
    expectTrue(fuse::renderer::DdgiIrradianceEncoding::isEmptyDirection({0.f, 0.f, 0.f}),
               "zero direction is empty");
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

void testProbeCacheIndexGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    std::vector<fuse::renderer::IrradianceCacheEntry> cache(8);
    for (fuse::u32 i = 0; i < 8u; ++i) {
        cache[i].irradiance = {static_cast<fuse::f32>(i + 1u), 0.f, 0.f};
    }

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
               "undersized cache reports undersized_cache reason");
    expectTrue(std::strcmp(fuse::renderer::probeCacheLookupRejectReasonLabel(reason), "undersized_cache") == 0,
               "undersized cache reject label");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
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

    fuse::math::Vec3 irradiance{};
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
}

void testDdgiLaunchGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    fuse::u32 indices[4] = {0u, 1u, 2u, 3u};

    expectTrue(fuse::renderer::canLaunchDdgiProbeUpdate(desc, indices, 4u),
               "valid launch params pass preflight");
    fuse::renderer::DdgiLaunchRejectReason reason = fuse::renderer::DdgiLaunchRejectReason::None;
    expectTrue(fuse::renderer::preflightDdgiProbeUpdate(desc, indices, 4u, reason),
               "preflightDdgiProbeUpdate succeeds for valid params");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::None, "valid launch reports no reject reason");

    fuse::renderer::DDGIDesc empty{};
    empty.grid_dims = {0, 2, 2};
    expectTrue(!fuse::renderer::preflightDdgiProbeUpdate(empty, indices, 4u, reason),
               "empty grid launch preflight fails");
    expectTrue(reason == fuse::renderer::DdgiLaunchRejectReason::EmptyGrid,
               "empty grid reports empty_grid launch reason");
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

    fuse::renderer::gi::DDGIKernelParams params{};
    params.probe_indices_to_update = indices;
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

    params.probe_indices_to_update = indices;
    params.probe_update_count = 0u;
    expectTrue(!fuse::renderer::gi::preflightProbeBlendKernel(params, kernelReason),
               "kernel blend preflight rejects zero update count");
    expectTrue(kernelReason == fuse::renderer::gi::DdgiKernelLaunchRejectReason::ZeroUpdateCount,
               "kernel blend reports zero_update_count reason");
}

void testSampleGuards() {
    fuse::renderer::DDGIDesc desc{};
    desc.grid_dims = {2, 2, 2};
    desc.irradiance_res = 8;

    expectTrue(fuse::renderer::ddgi_util::canSampleProbeGrid(desc), "default grid is sampleable");
    expectTrue(fuse::renderer::ddgi_util::requiredCacheCount(desc) == 8u, "required cache count matches probe count");
    expectTrue(fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 8u), "full cache sized for grid");
    expectTrue(!fuse::renderer::ddgi_util::isCacheSizedForGrid(desc, 4u), "undersized cache rejected");
    expectTrue(fuse::renderer::ddgi_util::cacheEntriesMissing(desc, 8u) == 0u, "full cache has zero missing entries");
    expectTrue(fuse::renderer::ddgi_util::cacheEntriesMissing(desc, 4u) == 4u, "undersized cache reports shortfall");

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
    testProbeIndexClamp();
    testProbeSampleCoords();
    testEmptyProbeGrid();
    testProbeValidityFlags();
    testProbePerAxisClamp();
    testClampProbeSampleCoords();
    testProbeSampleCoordsGuards();
    testProbeCacheIndexGuards();
    testDdgiLaunchGuards();
    testProbeBorderCounts();
    testResolveSampleDirectionFromSurface();
    testEmptyDirectionGuards();
    testSampleGuards();
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
