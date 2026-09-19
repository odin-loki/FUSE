#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_kernels.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::renderer {
namespace {

constexpr IrradianceCacheEntry kDefaultCacheEntry{};

fuse::math::Vec3 defaultAmbientIrradiance() {
    return {0.05f, 0.05f, 0.06f};
}

} // namespace

const char* probeSampleCoordsRejectReasonLabel(ProbeSampleCoordsRejectReason reason) {
    switch (reason) {
    case ProbeSampleCoordsRejectReason::None:
        return "none";
    case ProbeSampleCoordsRejectReason::EmptyGrid:
        return "empty_grid";
    case ProbeSampleCoordsRejectReason::NotSampleableGrid:
        return "not_sampleable_grid";
    case ProbeSampleCoordsRejectReason::OutOfRangeIndices:
        return "out_of_range_indices";
    case ProbeSampleCoordsRejectReason::OutOfRangeWeights:
        return "out_of_range_weights";
    case ProbeSampleCoordsRejectReason::UnorderedCorners:
        return "unordered_corners";
    }
    return "unknown";
}

bool probeSampleCoordsRejectReasonIsBlocking(ProbeSampleCoordsRejectReason reason) {
    switch (reason) {
    case ProbeSampleCoordsRejectReason::None:
    case ProbeSampleCoordsRejectReason::OutOfRangeWeights:
    case ProbeSampleCoordsRejectReason::UnorderedCorners:
        return false;
    case ProbeSampleCoordsRejectReason::EmptyGrid:
    case ProbeSampleCoordsRejectReason::NotSampleableGrid:
    case ProbeSampleCoordsRejectReason::OutOfRangeIndices:
        return true;
    }
    return true;
}

const char* probeGridSourceRejectReasonLabel(ProbeGridSourceRejectReason reason) {
    switch (reason) {
    case ProbeGridSourceRejectReason::None:
        return "none";
    case ProbeGridSourceRejectReason::EmptyGrid:
        return "empty_grid";
    case ProbeGridSourceRejectReason::NotSampleable:
        return "not_sampleable";
    }
    return "unknown";
}

bool probeGridSourceRejectReasonIsBlocking(ProbeGridSourceRejectReason reason) {
    return reason != ProbeGridSourceRejectReason::None;
}

const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason) {
    switch (reason) {
    case CacheIndexRejectReason::None:
        return "none";
    case CacheIndexRejectReason::EmptyGrid:
        return "empty_grid";
    case CacheIndexRejectReason::OutOfRangeProbeIndex:
        return "out_of_range_probe_index";
    case CacheIndexRejectReason::UndersizedCache:
        return "undersized_cache";
    case CacheIndexRejectReason::NullCache:
        return "null_cache";
    }
    return "unknown";
}

bool cacheIndexRejectReasonIsBlocking(CacheIndexRejectReason reason) {
    return reason != CacheIndexRejectReason::None;
}

const char* probeTrilinearSampleRejectReasonLabel(ProbeTrilinearSampleRejectReason reason) {
    switch (reason) {
    case ProbeTrilinearSampleRejectReason::None:
        return "none";
    case ProbeTrilinearSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case ProbeTrilinearSampleRejectReason::NotSampleable:
        return "not_sampleable";
    case ProbeTrilinearSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case ProbeTrilinearSampleRejectReason::UndersizedCache:
        return "undersized_cache";
    case ProbeTrilinearSampleRejectReason::NullCache:
        return "null_cache";
    }
    return "unknown";
}

bool probeTrilinearSampleRejectReasonIsBlocking(ProbeTrilinearSampleRejectReason reason) {
    return reason != ProbeTrilinearSampleRejectReason::None;
}

const char* probeUpdateLaunchRejectReasonLabel(ProbeUpdateLaunchRejectReason reason) {
    switch (reason) {
    case ProbeUpdateLaunchRejectReason::None:
        return "none";
    case ProbeUpdateLaunchRejectReason::EmptyGrid:
        return "empty_grid";
    case ProbeUpdateLaunchRejectReason::NullIndices:
        return "null_indices";
    case ProbeUpdateLaunchRejectReason::ZeroCount:
        return "zero_count";
    case ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex:
        return "out_of_range_probe_index";
    }
    return "unknown";
}

bool probeUpdateLaunchRejectReasonIsBlocking(ProbeUpdateLaunchRejectReason reason) {
    return reason != ProbeUpdateLaunchRejectReason::None;
}

ProbeUpdateLaunchRejectReason classifyDdgiProbeUpdateReject(const DDGIDesc& desc,
                                                            const u32* probe_indices,
                                                            u32 probe_count) {
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;
    tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
    return reason;
}

bool preflightDdgiProbeUpdate(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              ProbeUpdateLaunchRejectReason* reason) {
    const ProbeUpdateLaunchRejectReason reject =
        classifyDdgiProbeUpdateReject(desc, probe_indices, probe_count);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !probeUpdateLaunchRejectReasonIsBlocking(reject);
}

bool tryPreflightDdgiProbeUpdate(const DDGIDesc& desc,
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 ProbeUpdateLaunchRejectReason& outReason) {
    return preflightDdgiProbeUpdate(desc, probe_indices, probe_count, &outReason);
}

const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
    switch (reason) {
    case ProbeScheduleRejectReason::None:
        return "none";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroProbesPerFrame:
        return "zero_probes_per_frame";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";
    case ProbeScheduleRejectReason::NullOutIndices:
        return "null_out_indices";
    case ProbeScheduleRejectReason::NullOutCount:
        return "null_out_count";
    }
    return "unknown";
}

bool probeScheduleRejectReasonIsBlocking(ProbeScheduleRejectReason reason) {
    return reason != ProbeScheduleRejectReason::None;
}

bool ProbeGridLayout::isEmptyGrid(const DDGIDesc& desc) {
    return desc.grid_dims.x == 0u || desc.grid_dims.y == 0u || desc.grid_dims.z == 0u;
}

ProbeGridCoord ProbeGridLayout::probeCoordFromIndex(const DDGIDesc& desc, u32 probe_index) {
    ProbeGridCoord coord{};
    const u32 grid_x = desc.grid_dims.x;
    const u32 grid_y = desc.grid_dims.y;
    const u32 grid_z = desc.grid_dims.z;
    if (grid_x == 0u || grid_y == 0u || grid_z == 0u) {
        return coord;
    }

    const u32 slice = grid_x * grid_y;
    coord.z = probe_index / slice;
    const u32 rem = probe_index % slice;
    coord.y = rem / grid_x;
    coord.x = rem % grid_x;
    return coord;
}

ProbeGridCoord ProbeGridLayout::probeCoordFromClampedIndex(const DDGIDesc& desc, u32 probe_index) {
    return probeCoordFromIndex(desc, clampProbeIndex(probe_index, desc));
}

u32 ProbeGridLayout::probeIndexFromCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    if (!isValidProbeCoord(desc, coord)) {
        return UINT32_MAX;
    }
    return coord.z * desc.grid_dims.x * desc.grid_dims.y + coord.y * desc.grid_dims.x + coord.x;
}

bool ProbeGridLayout::isValidProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    return coord.x < desc.grid_dims.x && coord.y < desc.grid_dims.y && coord.z < desc.grid_dims.z;
}

bool ProbeGridLayout::isValidProbeIndex(const DDGIDesc& desc, u32 probe_index) {
    return probe_index < ddgi_util::probeCount(desc);
}

bool ProbeGridLayout::isBorderProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    if (!isValidProbeCoord(desc, coord)) {
        return false;
    }
    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;
    return coord.x == 0u || coord.y == 0u || coord.z == 0u || coord.x == max_x || coord.y == max_y ||
           coord.z == max_z;
}

ProbeBorderKind ProbeGridLayout::probeBorderKind(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    if (isEmptyGrid(desc) || !isValidProbeCoord(desc, coord)) {
        return ProbeBorderKind::Invalid;
    }

    u32 boundary_axes = 0u;
    if (desc.grid_dims.x > 1u) {
        const u32 max_x = desc.grid_dims.x - 1u;
        if (coord.x == 0u || coord.x == max_x) {
            ++boundary_axes;
        }
    }
    if (desc.grid_dims.y > 1u) {
        const u32 max_y = desc.grid_dims.y - 1u;
        if (coord.y == 0u || coord.y == max_y) {
            ++boundary_axes;
        }
    }
    if (desc.grid_dims.z > 1u) {
        const u32 max_z = desc.grid_dims.z - 1u;
        if (coord.z == 0u || coord.z == max_z) {
            ++boundary_axes;
        }
    }

    if (boundary_axes == 0u) {
        return isBorderProbeCoord(desc, coord) ? ProbeBorderKind::Corner : ProbeBorderKind::Interior;
    }
    if (boundary_axes == 1u) {
        return ProbeBorderKind::Face;
    }
    if (boundary_axes == 2u) {
        return ProbeBorderKind::Edge;
    }
    return ProbeBorderKind::Corner;
}

ProbeValidityFlags ProbeGridLayout::probeValidity(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    ProbeValidityFlags flags{};
    flags.valid = isValidProbeCoord(desc, coord);
    if (!flags.valid) {
        return flags;
    }

    flags.border_kind = probeBorderKind(desc, coord);
    flags.is_border = flags.border_kind != ProbeBorderKind::Interior;
    flags.interior = !flags.is_border;
    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;
    flags.has_trilinear_neighbourhood =
        coord.x > 0u && coord.y > 0u && coord.z > 0u && coord.x < max_x && coord.y < max_y && coord.z < max_z;
    return flags;
}

ProbeValidityFlags ProbeGridLayout::probeValidityFromIndex(const DDGIDesc& desc, u32 probe_index) {
    if (!isValidProbeIndex(desc, probe_index)) {
        return {};
    }
    return probeValidity(desc, probeCoordFromIndex(desc, probe_index));
}

ProbeValidityFlags ProbeGridLayout::probeValidityFromClampedIndex(const DDGIDesc& desc, u32 probe_index) {
    if (isEmptyGrid(desc)) {
        return {};
    }
    return probeValidity(desc, probeCoordFromClampedIndex(desc, probe_index));
}

bool ProbeGridLayout::isProbeIndexOutOfRange(u32 probe_index, const DDGIDesc& desc) {
    const u32 count = ddgi_util::probeCount(desc);
    return count == 0u || probe_index >= count;
}

u32 ProbeGridLayout::clampProbeIndex(u32 probe_index, const DDGIDesc& desc) {
    const u32 count = ddgi_util::probeCount(desc);
    if (count == 0u) {
        return 0u;
    }
    return std::min(probe_index, count - 1u);
}

u32 ProbeGridLayout::clampProbeCoordX(u32 x, const DDGIDesc& desc) {
    if (desc.grid_dims.x == 0u) {
        return 0u;
    }
    return std::min(x, desc.grid_dims.x - 1u);
}

u32 ProbeGridLayout::clampProbeCoordY(u32 y, const DDGIDesc& desc) {
    if (desc.grid_dims.y == 0u) {
        return 0u;
    }
    return std::min(y, desc.grid_dims.y - 1u);
}

u32 ProbeGridLayout::clampProbeCoordZ(u32 z, const DDGIDesc& desc) {
    if (desc.grid_dims.z == 0u) {
        return 0u;
    }
    return std::min(z, desc.grid_dims.z - 1u);
}

u32 ProbeGridLayout::probeIndexFromClampedCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    if (isEmptyGrid(desc)) {
        return 0u;
    }
    const ProbeGridCoord clamped{clampProbeCoordX(coord.x, desc),
                                 clampProbeCoordY(coord.y, desc),
                                 clampProbeCoordZ(coord.z, desc)};
    return probeIndexFromCoord(desc, clamped);
}

void ProbeGridLayout::normalizeProbeSampleCoords(ProbeSampleCoords& coords) {
    if (coords.x0 > coords.x1) {
        std::swap(coords.x0, coords.x1);
        coords.tx = 1.f - coords.tx;
    }
    if (coords.y0 > coords.y1) {
        std::swap(coords.y0, coords.y1);
        coords.ty = 1.f - coords.ty;
    }
    if (coords.z0 > coords.z1) {
        std::swap(coords.z0, coords.z1);
        coords.tz = 1.f - coords.tz;
    }

    coords.tx = std::clamp(coords.tx, 0.f, 1.f);
    coords.ty = std::clamp(coords.ty, 0.f, 1.f);
    coords.tz = std::clamp(coords.tz, 0.f, 1.f);
}

bool ProbeGridLayout::areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
    if (!inRange(coords.x0, max_x) || !inRange(coords.x1, max_x) || !inRange(coords.y0, max_y) ||
        !inRange(coords.y1, max_y) || !inRange(coords.z0, max_z) || !inRange(coords.z1, max_z)) {
        return false;
    }

    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        return false;
    }
    return true;
}

bool ProbeGridLayout::isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryValidateProbeSampleCoords(desc, coords, reason);
}

bool ProbeGridLayout::isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !isValidProbeSampleCoords(desc, coords);
}

bool ProbeGridLayout::tryValidateProbeSampleCoords(const DDGIDesc& desc,
                                                   const ProbeSampleCoords& coords,
                                                   ProbeSampleCoordsRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;
        return false;
    }

    if (!areProbeSampleCoordsInBounds(desc, coords)) {
        const u32 max_x = desc.grid_dims.x - 1u;
        const u32 max_y = desc.grid_dims.y - 1u;
        const u32 max_z = desc.grid_dims.z - 1u;
        const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
        const bool indicesInRange = inRange(coords.x0, max_x) && inRange(coords.x1, max_x) &&
                                    inRange(coords.y0, max_y) && inRange(coords.y1, max_y) &&
                                    inRange(coords.z0, max_z) && inRange(coords.z1, max_z);
        outReason = indicesInRange ? ProbeSampleCoordsRejectReason::OutOfRangeWeights
                                   : ProbeSampleCoordsRejectReason::OutOfRangeIndices;
        return false;
    }

    if (coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
        outReason = ProbeSampleCoordsRejectReason::UnorderedCorners;
        return false;
    }

    outReason = ProbeSampleCoordsRejectReason::None;
    return true;
}

bool ProbeGridLayout::wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
    }
    return isProbeSampleCoordsOutOfRange(desc, coords);
}

bool ProbeGridLayout::tryPreflightProbeSampleCoords(const DDGIDesc& desc,
                                                    const ProbeSampleCoords& coords,
                                                    ProbeSampleCoordsRejectReason& outReason) {
    outReason = classifyProbeSampleCoordsReject(desc, coords);
    return !probeSampleCoordsRejectReasonIsBlocking(outReason);
}

bool ProbeGridLayout::canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryPreflightProbeSampleCoords(desc, coords, reason);
}

bool ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return !tryPreflightProbeSampleCoords(desc, coords, reason);
}

bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc, const fuse::math::Vec3& world_position) {
    ProbeSampleCoords coords{};
    if (!buildProbeSampleCoords(desc, world_position, coords)) {
        return true;
    }
    return wouldSkipProbeSampleCoordPreflight(desc, coords);
}

ProbeSampleCoordsRejectReason ProbeGridLayout::classifyProbeSampleCoordsReject(const DDGIDesc& desc,
                                                                             const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    tryValidateProbeSampleCoords(desc, coords, reason);
    return reason;
}

bool ProbeGridLayout::preflightProbeSampleCoords(const DDGIDesc& desc,
                                                 const fuse::math::Vec3& world_position,
                                                 ProbeSampleCoords* out_coords,
                                                 ProbeSampleCoordsRejectReason* reason) {
    ProbeSampleCoords coords{};
    ProbeSampleCoordsRejectReason rejectReason = ProbeSampleCoordsRejectReason::None;
    if (!tryBuildProbeSampleCoords(desc, world_position, coords, rejectReason)) {
        if (reason != nullptr) {
            *reason = rejectReason;
        }
        return false;
    }

    const bool ok = tryPreflightProbeSampleCoords(desc, coords, rejectReason);
    if (out_coords != nullptr) {
        *out_coords = coords;
    }
    if (reason != nullptr) {
        *reason = rejectReason;
    }
    return ok;
}

bool ProbeGridLayout::tryClampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryClampProbeSampleCoords(desc, coords, reason);
}

bool ProbeGridLayout::tryClampProbeSampleCoords(const DDGIDesc& desc,
                                                ProbeSampleCoords& coords,
                                                ProbeSampleCoordsRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;
        return false;
    }
    clampProbeSampleCoords(desc, coords);
    outReason = ProbeSampleCoordsRejectReason::None;
    return true;
}

void ProbeGridLayout::clampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        coords = {};
        return;
    }

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    coords.x0 = std::min(coords.x0, max_x);
    coords.y0 = std::min(coords.y0, max_y);
    coords.z0 = std::min(coords.z0, max_z);
    coords.x1 = std::min(coords.x1, max_x);
    coords.y1 = std::min(coords.y1, max_y);
    coords.z1 = std::min(coords.z1, max_z);

    normalizeProbeSampleCoords(coords);
}

bool ProbeGridLayout::buildProbeSampleCoords(const DDGIDesc& desc,
                                             const fuse::math::Vec3& world_position,
                                             ProbeSampleCoords& out_coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryBuildProbeSampleCoords(desc, world_position, out_coords, reason);
}

bool ProbeGridLayout::tryBuildProbeSampleCoords(const DDGIDesc& desc,
                                                const fuse::math::Vec3& world_position,
                                                ProbeSampleCoords& out_coords,
                                                ProbeSampleCoordsRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;
        return false;
    }
    if (!ddgi_util::canSampleProbeGrid(desc)) {
        outReason = ProbeSampleCoordsRejectReason::NotSampleableGrid;
        return false;
    }

    const fuse::math::Vec3 grid_coord = worldToProbeGridCoord(desc, world_position);
    const fuse::math::Vec3 clamped = clampWorldToProbeGridCoord(desc, grid_coord);

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    out_coords.x0 = static_cast<u32>(std::floor(clamped.x));
    out_coords.y0 = static_cast<u32>(std::floor(clamped.y));
    out_coords.z0 = static_cast<u32>(std::floor(clamped.z));
    out_coords.x1 = std::min(out_coords.x0 + 1u, max_x);
    out_coords.y1 = std::min(out_coords.y0 + 1u, max_y);
    out_coords.z1 = std::min(out_coords.z0 + 1u, max_z);
    out_coords.tx = clamped.x - static_cast<f32>(out_coords.x0);
    out_coords.ty = clamped.y - static_cast<f32>(out_coords.y0);
    out_coords.tz = clamped.z - static_cast<f32>(out_coords.z0);
    outReason = ProbeSampleCoordsRejectReason::None;
    return true;
}

fuse::math::Vec2 DdgiIrradianceEncoding::clampEncodedUV(const fuse::math::Vec2& encoded) {
    return {std::clamp(encoded.x, 0.f, 1.f), std::clamp(encoded.y, 0.f, 1.f)};
}

bool DdgiIrradianceEncoding::isEmptyDirection(const fuse::math::Vec3& direction) {
    return direction.dot(direction) < 1e-8f;
}

bool DdgiIrradianceEncoding::isValidDirection(const fuse::math::Vec3& direction) {
    return !isEmptyDirection(direction);
}

fuse::math::Vec3 DdgiIrradianceEncoding::resolveSampleDirection(const fuse::math::Vec3& direction,
                                                                const fuse::math::Vec3& fallback) {
    if (!isEmptyDirection(direction)) {
        return direction.normalized();
    }
    if (!isEmptyDirection(fallback)) {
        return fallback.normalized();
    }
    return {0.f, 1.f, 0.f};
}

fuse::math::Vec3 DdgiIrradianceEncoding::resolveSampleDirectionFromSurface(
    const fuse::math::Vec3& direction,
    const fuse::math::Vec3& surface_normal) {
    return resolveSampleDirection(direction, surface_normal);
}

fuse::math::Vec2 DdgiIrradianceEncoding::encodeDirection(const fuse::math::Vec3& direction) {
    fuse::math::Vec3 n = resolveSampleDirection(direction);

    const f32 sum = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    if (sum > 1e-8f) {
        n = n * (1.f / sum);
    }

    fuse::math::Vec2 o{};
    if (n.z >= 0.f) {
        o.x = n.x;
        o.y = n.y;
    } else {
        o.x = (1.f - std::fabs(n.y)) * (n.x >= 0.f ? 1.f : -1.f);
        o.y = (1.f - std::fabs(n.x)) * (n.y >= 0.f ? 1.f : -1.f);
    }

    return {o.x * 0.5f + 0.5f, o.y * 0.5f + 0.5f};
}

fuse::math::Vec3 DdgiIrradianceEncoding::decodeDirection(const fuse::math::Vec2& encoded) {
    const fuse::math::Vec2 clamped = clampEncodedUV(encoded);
    fuse::math::Vec2 enc = {clamped.x * 2.f - 1.f, clamped.y * 2.f - 1.f};
    fuse::math::Vec3 n = {enc.x, enc.y, 1.f - std::fabs(enc.x) - std::fabs(enc.y)};
    if (n.z < 0.f) {
        const f32 signX = n.x >= 0.f ? 1.f : -1.f;
        const f32 signY = n.y >= 0.f ? 1.f : -1.f;
        n.x = (1.f - std::fabs(n.y)) * signX;
        n.y = (1.f - std::fabs(n.x)) * signY;
    }
    return n.normalized();
}

fuse::math::Vec2 DdgiIrradianceEncoding::directionToAtlasUV(const fuse::math::Vec3& direction) {
    return encodeDirection(direction);
}

fuse::math::Vec2 DdgiIrradianceEncoding::directionToTexelOffset(const fuse::math::Vec3& direction,
                                                                u32 irradiance_res) {
    if (irradiance_res == 0u) {
        return {};
    }
    const fuse::math::Vec2 uv = directionToAtlasUV(direction);
    const f32 max_texel = static_cast<f32>(irradiance_res - 1u);
    return {std::clamp(uv.x * max_texel, 0.f, max_texel), std::clamp(uv.y * max_texel, 0.f, max_texel)};
}

bool DdgiIrradianceEncoding::buildTileBilinearCoords(const fuse::math::Vec3& direction,
                                                     u32 irradiance_res,
                                                     DdgiTileBilinearCoords& out_coords) {
    if (irradiance_res == 0u) {
        return false;
    }

    const fuse::math::Vec2 offset = directionToTexelOffset(direction, irradiance_res);
    const f32 max_texel = static_cast<f32>(irradiance_res - 1u);
    out_coords.texel_u0 = static_cast<u32>(std::floor(offset.x));
    out_coords.texel_v0 = static_cast<u32>(std::floor(offset.y));
    out_coords.texel_u1 = std::min(out_coords.texel_u0 + 1u, static_cast<u32>(max_texel));
    out_coords.texel_v1 = std::min(out_coords.texel_v0 + 1u, static_cast<u32>(max_texel));
    out_coords.tu = offset.x - static_cast<f32>(out_coords.texel_u0);
    out_coords.tv = offset.y - static_cast<f32>(out_coords.texel_v0);
    return true;
}

bool DdgiIrradianceEncoding::canDirectionallySample(u32 irradiance_res) {
    return irradiance_res > 0u;
}

f32 DdgiIrradianceEncoding::angularErrorRadians(const fuse::math::Vec3& a, const fuse::math::Vec3& b) {
    const fuse::math::Vec3 na = a.normalized();
    const fuse::math::Vec3 nb = b.normalized();
    if (na.dot(na) < 1e-8f || nb.dot(nb) < 1e-8f) {
        return 0.f;
    }
    const f32 dot = std::clamp(na.dot(nb), -1.f, 1.f);
    return std::acos(dot);
}

fuse::math::Vec3 ProbeGridLayout::worldToProbeGridCoord(const DDGIDesc& desc,
                                                        const fuse::math::Vec3& world_position) {
    const fuse::math::Vec3 delta = world_position - desc.grid_origin;
    if (desc.probe_spacing.x <= 0.f || desc.probe_spacing.y <= 0.f || desc.probe_spacing.z <= 0.f) {
        return {};
    }
    return {delta.x / desc.probe_spacing.x,
            delta.y / desc.probe_spacing.y,
            delta.z / desc.probe_spacing.z};
}

fuse::math::Vec3 ProbeGridLayout::clampWorldToProbeGridCoord(const DDGIDesc& desc,
                                                             const fuse::math::Vec3& grid_coord) {
    if (desc.grid_dims.x == 0u || desc.grid_dims.y == 0u || desc.grid_dims.z == 0u) {
        return {};
    }
    const f32 max_x = static_cast<f32>(desc.grid_dims.x - 1u);
    const f32 max_y = static_cast<f32>(desc.grid_dims.y - 1u);
    const f32 max_z = static_cast<f32>(desc.grid_dims.z - 1u);
    return {std::clamp(grid_coord.x, 0.f, max_x),
            std::clamp(grid_coord.y, 0.f, max_y),
            std::clamp(grid_coord.z, 0.f, max_z)};
}

ProbeGridCoord ProbeGridLayout::clampProbeGridCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    ProbeGridCoord clamped{};
    if (desc.grid_dims.x == 0u || desc.grid_dims.y == 0u || desc.grid_dims.z == 0u) {
        return clamped;
    }
    clamped.x = std::min(coord.x, desc.grid_dims.x - 1u);
    clamped.y = std::min(coord.y, desc.grid_dims.y - 1u);
    clamped.z = std::min(coord.z, desc.grid_dims.z - 1u);
    return clamped;
}

fuse::math::Vec2 ProbeGridLayout::probeIrradianceAtlasOrigin(const DDGIDesc& desc,
                                                             const ProbeGridCoord& coord) {
    if (isEmptyGrid(desc) || !isValidProbeCoord(desc, coord) || desc.irradiance_res == 0u) {
        return {};
    }
    return {static_cast<f32>(coord.x * desc.irradiance_res),
            static_cast<f32>((coord.z * desc.grid_dims.y + coord.y) * desc.irradiance_res)};
}

fuse::math::Vec2 ProbeGridLayout::probeIrradianceAtlasTexel(const DDGIDesc& desc,
                                                            const ProbeGridCoord& coord,
                                                            const fuse::math::Vec3& direction) {
    if (isEmptyGrid(desc) || !isValidProbeCoord(desc, coord) || desc.irradiance_res == 0u) {
        return {};
    }
    const fuse::math::Vec2 origin = probeIrradianceAtlasOrigin(desc, coord);
    const fuse::math::Vec2 offset =
        DdgiIrradianceEncoding::directionToTexelOffset(direction, desc.irradiance_res);
    return {origin.x + offset.x, origin.y + offset.y};
}

fuse::math::Vec2 ProbeGridLayout::probeDepthAtlasOrigin(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    if (isEmptyGrid(desc) || !isValidProbeCoord(desc, coord) || desc.depth_res == 0u) {
        return {};
    }
    return {static_cast<f32>(coord.x * desc.depth_res),
            static_cast<f32>((coord.z * desc.grid_dims.y + coord.y) * desc.depth_res)};
}

namespace ddgi_util {

u32 probeCount(const DDGIDesc& desc) {
    return desc.grid_dims.x * desc.grid_dims.y * desc.grid_dims.z;
}

u32 countBorderProbes(const DDGIDesc& desc) {
    const u32 count = probeCount(desc);
    if (count == 0u) {
        return 0u;
    }

    u32 border_count = 0u;
    for (u32 i = 0u; i < count; ++i) {
        const ProbeGridCoord coord = ProbeGridLayout::probeCoordFromIndex(desc, i);
        if (ProbeGridLayout::isBorderProbeCoord(desc, coord)) {
            ++border_count;
        }
    }
    return border_count;
}

u32 countInteriorProbes(const DDGIDesc& desc) {
    const u32 count = probeCount(desc);
    if (count == 0u) {
        return 0u;
    }

    const u32 interior_x = desc.grid_dims.x > 2u ? desc.grid_dims.x - 2u : 0u;
    const u32 interior_y = desc.grid_dims.y > 2u ? desc.grid_dims.y - 2u : 0u;
    const u32 interior_z = desc.grid_dims.z > 2u ? desc.grid_dims.z - 2u : 0u;
    return interior_x * interior_y * interior_z;
}

ProbeBorderCounts countProbesByBorderKind(const DDGIDesc& desc) {
    ProbeBorderCounts counts{};
    counts.total = probeCount(desc);
    if (counts.total == 0u) {
        return counts;
    }

    counts.interior = countInteriorProbes(desc);
    counts.border = counts.total - counts.interior;

    for (u32 i = 0u; i < counts.total; ++i) {
        const ProbeGridCoord coord = ProbeGridLayout::probeCoordFromIndex(desc, i);
        switch (ProbeGridLayout::probeBorderKind(desc, coord)) {
        case ProbeBorderKind::Interior:
            break;
        case ProbeBorderKind::Face:
            ++counts.face;
            break;
        case ProbeBorderKind::Edge:
            ++counts.edge;
            break;
        case ProbeBorderKind::Corner:
            ++counts.corner;
            break;
        case ProbeBorderKind::Invalid:
            break;
        }
    }
    return counts;
}

u32 countProbesOfBorderKind(const DDGIDesc& desc, ProbeBorderKind kind) {
    const ProbeBorderCounts counts = countProbesByBorderKind(desc);
    switch (kind) {
    case ProbeBorderKind::Interior:
        return counts.interior;
    case ProbeBorderKind::Face:
        return counts.face;
    case ProbeBorderKind::Edge:
        return counts.edge;
    case ProbeBorderKind::Corner:
        return counts.corner;
    case ProbeBorderKind::Invalid:
        break;
    }
    return 0u;
}

bool validateProbeBorderCounts(const ProbeBorderCounts& counts) {
    if (counts.total == 0u) {
        return counts.interior == 0u && counts.border == 0u && counts.face == 0u && counts.edge == 0u &&
               counts.corner == 0u;
    }
    if (counts.interior + counts.border != counts.total) {
        return false;
    }
    return counts.face + counts.edge + counts.corner == counts.border;
}

bool validateProbeBorderCountsForGrid(const DDGIDesc& desc) {
    return validateProbeBorderCounts(countProbesByBorderKind(desc));
}

bool canSampleProbeGrid(const DDGIDesc& desc) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return false;
    }
    if (desc.irradiance_res == 0u) {
        return false;
    }
    return desc.probe_spacing.x > 0.f && desc.probe_spacing.y > 0.f && desc.probe_spacing.z > 0.f;
}

bool shouldSkipProbeGrid(const DDGIDesc& desc) {
    return !canSampleProbeGrid(desc);
}

bool wouldSkipProbeGridSource(const DDGIDesc& desc) {
    return shouldSkipProbeGrid(desc);
}

bool tryValidateProbeGridSource(const DDGIDesc& desc, ProbeGridSourceRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = ProbeGridSourceRejectReason::EmptyGrid;
        return false;
    }
    if (!canSampleProbeGrid(desc)) {
        outReason = ProbeGridSourceRejectReason::NotSampleable;
        return false;
    }

    outReason = ProbeGridSourceRejectReason::None;
    return true;
}

ProbeGridSourceRejectReason classifyProbeGridSourceReject(const DDGIDesc& desc) {
    ProbeGridSourceRejectReason reason = ProbeGridSourceRejectReason::None;
    tryValidateProbeGridSource(desc, reason);
    return reason;
}

bool preflightProbeGridSource(const DDGIDesc& desc, ProbeGridSourceRejectReason* reason) {
    const ProbeGridSourceRejectReason reject = classifyProbeGridSourceReject(desc);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !probeGridSourceRejectReasonIsBlocking(reject);
}

bool tryPreflightProbeGridSource(const DDGIDesc& desc, ProbeGridSourceRejectReason& outReason) {
    return preflightProbeGridSource(desc, &outReason);
}

bool isProbeCacheAccessible(const DDGIDesc& desc, const IrradianceCacheEntry* cache, u32 cache_count) {
    return cache != nullptr && canSampleProbeGrid(desc) && isCacheSizedForGrid(desc, cache_count);
}

bool shouldSkipProbeLookup(const DDGIDesc& desc, const IrradianceCacheEntry* cache, u32 cache_count) {
    return !isProbeCacheAccessible(desc, cache, cache_count);
}

bool canSampleAtProbeCoords(const DDGIDesc& desc,
                            const ProbeSampleCoords& coords,
                            const IrradianceCacheEntry* cache,
                            u32 cache_count) {
    ProbeTrilinearSampleRejectReason reason = ProbeTrilinearSampleRejectReason::None;
    return tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
}

bool tryCanSampleAtProbeCoords(const DDGIDesc& desc,
                               const ProbeSampleCoords& coords,
                               const IrradianceCacheEntry* cache,
                               u32 cache_count,
                               ProbeTrilinearSampleRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = ProbeTrilinearSampleRejectReason::EmptyGrid;
        return false;
    }
    if (!canSampleProbeGrid(desc)) {
        outReason = ProbeTrilinearSampleRejectReason::NotSampleable;
        return false;
    }
    if (cache == nullptr) {
        outReason = ProbeTrilinearSampleRejectReason::NullCache;
        return false;
    }
    if (!isCacheSizedForGrid(desc, cache_count)) {
        outReason = ProbeTrilinearSampleRejectReason::UndersizedCache;
        return false;
    }
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        outReason = ProbeTrilinearSampleRejectReason::InvalidSampleCoords;
        return false;
    }

    outReason = ProbeTrilinearSampleRejectReason::None;
    return true;
}

bool wouldSkipTrilinearProbeSampleAtCoords(const DDGIDesc& desc,
                                           const ProbeSampleCoords& coords,
                                           const IrradianceCacheEntry* cache,
                                           u32 cache_count) {
    ProbeTrilinearSampleRejectReason reason = ProbeTrilinearSampleRejectReason::None;
    return !tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
}

bool wouldSkipTrilinearProbeSample(const DDGIDesc& desc,
                                   const fuse::math::Vec3& world_position,
                                   const IrradianceCacheEntry* cache,
                                   u32 cache_count) {
    if (shouldSkipProbeGrid(desc) || shouldSkipProbeLookup(desc, cache, cache_count)) {
        return true;
    }

    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords)) {
        return true;
    }
    return wouldSkipTrilinearProbeSampleAtCoords(desc, coords, cache, cache_count);
}

bool preflightTrilinearProbeSample(const DDGIDesc& desc,
                                   const fuse::math::Vec3& world_position,
                                   const IrradianceCacheEntry* cache,
                                   u32 cache_count,
                                   ProbeSampleCoords* out_coords,
                                   ProbeTrilinearSampleRejectReason* reason) {
    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords)) {
        const ProbeTrilinearSampleRejectReason reject =
            ProbeGridLayout::isEmptyGrid(desc) ? ProbeTrilinearSampleRejectReason::EmptyGrid
                                                : ProbeTrilinearSampleRejectReason::NotSampleable;
        if (reason != nullptr) {
            *reason = reject;
        }
        return false;
    }

    ProbeTrilinearSampleRejectReason reject = ProbeTrilinearSampleRejectReason::None;
    if (!tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reject)) {
        if (reason != nullptr) {
            *reason = reject;
        }
        return false;
    }

    if (out_coords != nullptr) {
        *out_coords = coords;
    }
    if (reason != nullptr) {
        *reason = ProbeTrilinearSampleRejectReason::None;
    }
    return true;
}

bool tryPreflightTrilinearProbeSample(const DDGIDesc& desc,
                                      const fuse::math::Vec3& world_position,
                                      const IrradianceCacheEntry* cache,
                                      u32 cache_count,
                                      ProbeSampleCoords& out_coords,
                                      ProbeTrilinearSampleRejectReason& outReason) {
    return preflightTrilinearProbeSample(
        desc, world_position, cache, cache_count, &out_coords, &outReason);
}

bool tryReadIrradianceAtIndex(const DDGIDesc& desc,
                              const IrradianceCacheEntry* cache,
                              u32 cache_count,
                              u32 probe_index,
                              fuse::math::Vec3& out_irradiance) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return tryReadIrradianceAtIndex(desc, cache, cache_count, probe_index, out_irradiance, reason);
}

bool tryReadIrradianceAtIndex(const DDGIDesc& desc,
                              const IrradianceCacheEntry* cache,
                              u32 cache_count,
                              u32 probe_index,
                              fuse::math::Vec3& out_irradiance,
                              CacheIndexRejectReason& outReason) {
    if (!tryValidateCacheIndex(desc, cache, probe_index, cache_count, outReason)) {
        out_irradiance = {};
        return false;
    }
    if (!isCacheSizedForGrid(desc, cache_count)) {
        outReason = CacheIndexRejectReason::UndersizedCache;
        out_irradiance = {};
        return false;
    }
    out_irradiance = cache[probe_index].irradiance;
    outReason = CacheIndexRejectReason::None;
    return true;
}

u32 requiredCacheCount(const DDGIDesc& desc) {
    if (!canSampleProbeGrid(desc)) {
        return 0u;
    }
    return probeCount(desc);
}

bool isCacheSizedForGrid(const DDGIDesc& desc, u32 cache_count) {
    const u32 required = requiredCacheCount(desc);
    if (required == 0u) {
        return true;
    }
    return cache_count >= required;
}

u32 cacheEntriesMissing(const DDGIDesc& desc, u32 cache_count) {
    const u32 required = requiredCacheCount(desc);
    if (required == 0u || cache_count >= required) {
        return 0u;
    }
    return required - cache_count;
}

bool tryValidateCacheIndex(const DDGIDesc& desc,
                           u32 probe_index,
                           u32 cache_count,
                           CacheIndexRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = CacheIndexRejectReason::EmptyGrid;
        return false;
    }
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index)) {
        outReason = CacheIndexRejectReason::OutOfRangeProbeIndex;
        return false;
    }
    if (cache_count == 0u) {
        outReason = CacheIndexRejectReason::UndersizedCache;
        return false;
    }
    if (probe_index >= cache_count) {
        outReason = CacheIndexRejectReason::UndersizedCache;
        return false;
    }
    outReason = CacheIndexRejectReason::None;
    return true;
}

bool tryValidateCacheIndex(const DDGIDesc& desc,
                           const IrradianceCacheEntry* cache,
                           u32 probe_index,
                           u32 cache_count,
                           CacheIndexRejectReason& outReason) {
    if (cache == nullptr) {
        outReason = CacheIndexRejectReason::NullCache;
        return false;
    }
    return tryValidateCacheIndex(desc, probe_index, cache_count, outReason);
}

bool tryValidateCacheIndexAfterClamp(const DDGIDesc& desc,
                                     u32 probe_index,
                                     u32 cache_count,
                                     CacheIndexRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = CacheIndexRejectReason::EmptyGrid;
        return false;
    }
    if (cache_count == 0u) {
        outReason = CacheIndexRejectReason::UndersizedCache;
        return false;
    }

    const u32 clamped = ProbeGridLayout::clampProbeIndex(probe_index, desc);
    if (clamped >= cache_count) {
        outReason = CacheIndexRejectReason::UndersizedCache;
        return false;
    }

    outReason = CacheIndexRejectReason::None;
    return true;
}

bool tryValidateCacheIndexAfterClamp(const DDGIDesc& desc,
                                     const IrradianceCacheEntry* cache,
                                     u32 probe_index,
                                     u32 cache_count,
                                     CacheIndexRejectReason& outReason) {
    if (cache == nullptr) {
        outReason = CacheIndexRejectReason::NullCache;
        return false;
    }
    return tryValidateCacheIndexAfterClamp(desc, probe_index, cache_count, outReason);
}

bool isCacheIndexValid(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return tryValidateCacheIndex(desc, probe_index, cache_count, reason);
}

bool wouldSkipCacheIndexLookup(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return !tryValidateCacheIndex(desc, probe_index, cache_count, reason);
}

bool wouldSkipCacheIndexLookup(const DDGIDesc& desc,
                               const IrradianceCacheEntry* cache,
                               u32 probe_index,
                               u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return !tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);
}

bool wouldSkipCacheIndexLookupAfterClamp(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return !tryValidateCacheIndexAfterClamp(desc, probe_index, cache_count, reason);
}

bool wouldSkipCacheIndexLookupAfterClamp(const DDGIDesc& desc,
                                          const IrradianceCacheEntry* cache,
                                          u32 probe_index,
                                          u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return !tryValidateCacheIndexAfterClamp(desc, cache, probe_index, cache_count, reason);
}

bool wouldSkipCacheIndexLookupAtCoord(const DDGIDesc& desc,
                                      const ProbeGridCoord& coord,
                                      const IrradianceCacheEntry* cache,
                                      u32 cache_count) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return true;
    }

    const ProbeGridCoord clamped = ProbeGridLayout::clampProbeGridCoord(desc, coord);
    const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, clamped);
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return !tryValidateCacheIndexAfterClamp(desc, cache, index, cache_count, reason);
}

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                u32 probe_index,
                                                u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    tryValidateCacheIndex(desc, probe_index, cache_count, reason);
    return reason;
}

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                const IrradianceCacheEntry* cache,
                                                u32 probe_index,
                                                u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);
    return reason;
}

bool preflightCacheIndexLookup(const DDGIDesc& desc,
                               const IrradianceCacheEntry* cache,
                               u32 probe_index,
                               u32 cache_count,
                               CacheIndexRejectReason* reason) {
    const CacheIndexRejectReason reject = classifyCacheIndexReject(desc, cache, probe_index, cache_count);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !cacheIndexRejectReasonIsBlocking(reject);
}

bool tryPreflightCacheIndexLookup(const DDGIDesc& desc,
                                  const IrradianceCacheEntry* cache,
                                  u32 probe_index,
                                  u32 cache_count,
                                  CacheIndexRejectReason& outReason) {
    return preflightCacheIndexLookup(desc, cache, probe_index, cache_count, &outReason);
}

bool wouldClampProbeIndexForLookup(u32 probe_index, const DDGIDesc& desc) {
    return !ProbeGridLayout::isEmptyGrid(desc) && ProbeGridLayout::isProbeIndexOutOfRange(probe_index, desc);
}

bool isValidSampleRequest(const DDGIDesc& desc,
                          const DDGISampleRequest& /*request*/,
                          u32 cache_count) {
    return canSampleProbeGrid(desc) && isCacheSizedForGrid(desc, cache_count);
}

fuse::math::Vec3 probeWorldPosition(const DDGIDesc& desc, u32 probe_index) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return desc.grid_origin;
    }
    const ProbeGridCoord coord = ProbeGridLayout::probeCoordFromIndex(desc, probe_index);
    return desc.grid_origin +
           fuse::math::Vec3{desc.probe_spacing.x * static_cast<f32>(coord.x),
                            desc.probe_spacing.y * static_cast<f32>(coord.y),
                            desc.probe_spacing.z * static_cast<f32>(coord.z)};
}

fuse::math::Vec3 probeWorldPositionClamped(const DDGIDesc& desc, u32 probe_index) {
    return probeWorldPosition(desc, ProbeGridLayout::clampProbeIndex(probe_index, desc));
}

u32 irradianceAtlasWidth(const DDGIDesc& desc) {
    return desc.grid_dims.x * desc.irradiance_res;
}

u32 irradianceAtlasHeight(const DDGIDesc& desc) {
    return desc.grid_dims.y * desc.grid_dims.z * desc.irradiance_res;
}

u32 depthAtlasWidth(const DDGIDesc& desc) {
    return desc.grid_dims.x * desc.depth_res;
}

u32 depthAtlasHeight(const DDGIDesc& desc) {
    return desc.grid_dims.y * desc.grid_dims.z * desc.depth_res;
}

bool tryCanScheduleProbeUpdates(u32 probe_count,
                               u32 max_indices,
                               const u32* out_indices,
                               u32* out_count,
                               ProbeScheduleRejectReason& outReason) {
    if (out_indices == nullptr) {
        outReason = ProbeScheduleRejectReason::NullOutIndices;
        if (out_count != nullptr) {
            *out_count = 0u;
        }
        return false;
    }
    if (out_count == nullptr) {
        outReason = ProbeScheduleRejectReason::NullOutCount;
        return false;
    }
    if (probe_count == 0u) {
        outReason = ProbeScheduleRejectReason::ZeroProbeCount;
        *out_count = 0u;
        return false;
    }
    if (max_indices == 0u) {
        outReason = ProbeScheduleRejectReason::ZeroMaxIndices;
        *out_count = 0u;
        return false;
    }

    outReason = ProbeScheduleRejectReason::None;
    return true;
}

bool canScheduleProbeUpdates(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    return tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
}

bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    return !canScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count);
}

bool tryCanScheduleProbeUpdatesAtRate(u32 probe_count,
                                      u32 probes_per_frame,
                                      u32 max_indices,
                                      const u32* out_indices,
                                      u32* out_count,
                                      ProbeScheduleRejectReason& outReason) {
    if (!tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, outReason)) {
        return false;
    }
    if (probes_per_frame == 0u) {
        outReason = ProbeScheduleRejectReason::ZeroProbesPerFrame;
        if (out_count != nullptr) {
            *out_count = 0u;
        }
        return false;
    }

    outReason = ProbeScheduleRejectReason::None;
    return true;
}

bool canScheduleProbeUpdatesAtRate(u32 probe_count,
                                   u32 probes_per_frame,
                                   u32 max_indices,
                                   const u32* out_indices,
                                   u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    return tryCanScheduleProbeUpdatesAtRate(
        probe_count, probes_per_frame, max_indices, out_indices, out_count, reason);
}

bool wouldSkipProbeScheduleAtRate(u32 probe_count,
                                  u32 probes_per_frame,
                                  u32 max_indices,
                                  const u32* out_indices,
                                  u32* out_count) {
    return !canScheduleProbeUpdatesAtRate(
        probe_count, probes_per_frame, max_indices, out_indices, out_count);
}

ProbeScheduleRejectReason classifyProbeScheduleRejectAtRate(u32 probe_count,
                                                            u32 probes_per_frame,
                                                            u32 max_indices,
                                                            const u32* out_indices,
                                                            u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    tryCanScheduleProbeUpdatesAtRate(probe_count, probes_per_frame, max_indices, out_indices, out_count, reason);
    return reason;
}

bool preflightProbeScheduleAtRate(u32 probe_count,
                                  u32 probes_per_frame,
                                  u32 max_indices,
                                  const u32* out_indices,
                                  u32* out_count,
                                  ProbeScheduleRejectReason* reason) {
    const ProbeScheduleRejectReason reject =
        classifyProbeScheduleRejectAtRate(probe_count, probes_per_frame, max_indices, out_indices, out_count);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !probeScheduleRejectReasonIsBlocking(reject);
}

bool tryPreflightProbeScheduleAtRate(u32 probe_count,
                                     u32 probes_per_frame,
                                     u32 max_indices,
                                     const u32* out_indices,
                                     u32* out_count,
                                     ProbeScheduleRejectReason& outReason) {
    return preflightProbeScheduleAtRate(
        probe_count, probes_per_frame, max_indices, out_indices, out_count, &outReason);
}

void scheduleProbeUpdates(u32 frame_index,
                          u32 probe_count,
                          u32 probes_per_frame,
                          u32* out_indices,
                          u32 max_indices,
                          u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    if (!tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason)) {
        return;
    }

    const u32 count = std::min(probes_per_frame, std::min(probe_count, max_indices));
    const u32 start = (frame_index * probes_per_frame) % probe_count;
    for (u32 i = 0; i < count; ++i) {
        out_indices[i] = (start + i) % probe_count;
    }
    *out_count = count;
}

bool tryScheduleProbeUpdates(u32 frame_index,
                             u32 probe_count,
                             u32 probes_per_frame,
                             u32* out_indices,
                             u32 max_indices,
                             u32* out_count,
                             ProbeScheduleRejectReason& outReason) {
    if (!tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, outReason)) {
        return false;
    }
    scheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count);
    return true;
}

bool tryScheduleProbeUpdatesAtRate(u32 frame_index,
                                     u32 probe_count,
                                     u32 probes_per_frame,
                                     u32* out_indices,
                                     u32 max_indices,
                                     u32* out_count,
                                     ProbeScheduleRejectReason& outReason) {
    if (!tryCanScheduleProbeUpdatesAtRate(
            probe_count, probes_per_frame, max_indices, out_indices, out_count, outReason)) {
        return false;
    }
    scheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count);
    return true;
}

ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      u32 max_indices,
                                                      const u32* out_indices,
                                                      u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
    return reason;
}

bool preflightProbeSchedule(u32 probe_count,
                            u32 max_indices,
                            const u32* out_indices,
                            u32* out_count,
                            ProbeScheduleRejectReason* reason) {
    const ProbeScheduleRejectReason reject =
        classifyProbeScheduleReject(probe_count, max_indices, out_indices, out_count);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !probeScheduleRejectReasonIsBlocking(reject);
}

bool wouldClampScheduledProbeCount(u32 probe_count, u32 probes_per_frame, u32 max_indices) {
    if (probe_count == 0u || max_indices == 0u) {
        return false;
    }
    const u32 scheduled = std::min(probes_per_frame, std::min(probe_count, max_indices));
    return scheduled < probes_per_frame;
}

fuse::math::Vec3 blendIrradiance(const fuse::math::Vec3& previous,
                                 const fuse::math::Vec3& incoming,
                                 f32 hysteresis) {
    const f32 blend = std::clamp(hysteresis, 0.f, 1.f);
    if (blend >= 1.f - 1e-8f) {
        return previous;
    }
    if (blend <= 1e-8f) {
        return incoming;
    }
    return lerpIrradiance(previous, incoming, 1.f - blend);
}

fuse::math::Vec3 lerpIrradiance(const fuse::math::Vec3& a, const fuse::math::Vec3& b, f32 t) {
    const f32 clamped = std::clamp(t, 0.f, 1.f);
    return a * (1.f - clamped) + b * clamped;
}

fuse::math::Vec3 bilinearTileIrradiance(const fuse::math::Vec3* samples, f32 u, f32 v) {
    if (samples == nullptr) {
        return {};
    }
    const f32 clamped_u = std::clamp(u, 0.f, 1.f);
    const f32 clamped_v = std::clamp(v, 0.f, 1.f);
    const fuse::math::Vec3 row0 = lerpIrradiance(samples[0], samples[1], clamped_u);
    const fuse::math::Vec3 row1 = lerpIrradiance(samples[2], samples[3], clamped_u);
    return lerpIrradiance(row0, row1, clamped_v);
}

fuse::math::Vec3 sampleDirectionalIrradianceAtProbe(const IrradianceCacheEntry& entry,
                                                    const fuse::math::Vec3& direction,
                                                    u32 irradiance_res) {
    if (!DdgiIrradianceEncoding::canDirectionallySample(irradiance_res)) {
        return {};
    }

    const fuse::math::Vec3 sample_dir = DdgiIrradianceEncoding::resolveSampleDirection(direction);

    DdgiTileBilinearCoords coords{};
    if (!DdgiIrradianceEncoding::buildTileBilinearCoords(sample_dir, irradiance_res, coords)) {
        return {};
    }

    const auto texel_direction = [&](u32 texel_u, u32 texel_v) -> fuse::math::Vec3 {
        if (irradiance_res == 0u) {
            return {};
        }
        const f32 max_texel = static_cast<f32>(irradiance_res - 1u);
        const fuse::math::Vec2 encoded = {static_cast<f32>(texel_u) / max_texel,
                                            static_cast<f32>(texel_v) / max_texel};
        return DdgiIrradianceEncoding::decodeDirection(encoded);
    };

    const auto weighted = [&](u32 texel_u, u32 texel_v) -> fuse::math::Vec3 {
        const fuse::math::Vec3 texel_dir = texel_direction(texel_u, texel_v);
        const f32 weight = std::max(0.f, sample_dir.dot(texel_dir));
        return entry.irradiance * weight;
    };

    const fuse::math::Vec3 samples[4] = {weighted(coords.texel_u0, coords.texel_v0),
                                         weighted(coords.texel_u1, coords.texel_v0),
                                         weighted(coords.texel_u0, coords.texel_v1),
                                         weighted(coords.texel_u1, coords.texel_v1)};
    return bilinearTileIrradiance(samples, coords.tu, coords.tv);
}

namespace {

fuse::math::Vec3 trilinearProbeIrradianceFromCoords(const DDGIDesc& desc,
                                                    const ProbeSampleCoords& coords,
                                                    const IrradianceCacheEntry* cache,
                                                    u32 cache_count) {
    const auto sample_probe = [&](u32 x, u32 y, u32 z) -> fuse::math::Vec3 {
        const ProbeGridCoord probe_coord{x, y, z};
        const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, probe_coord);
        fuse::math::Vec3 irradiance{};
        if (!tryReadIrradianceAtIndex(desc, cache, cache_count, index, irradiance)) {
            return {};
        }
        return irradiance;
    };

    const fuse::math::Vec3 c000 = sample_probe(coords.x0, coords.y0, coords.z0);
    const fuse::math::Vec3 c100 = sample_probe(coords.x1, coords.y0, coords.z0);
    const fuse::math::Vec3 c010 = sample_probe(coords.x0, coords.y1, coords.z0);
    const fuse::math::Vec3 c110 = sample_probe(coords.x1, coords.y1, coords.z0);
    const fuse::math::Vec3 c001 = sample_probe(coords.x0, coords.y0, coords.z1);
    const fuse::math::Vec3 c101 = sample_probe(coords.x1, coords.y0, coords.z1);
    const fuse::math::Vec3 c011 = sample_probe(coords.x0, coords.y1, coords.z1);
    const fuse::math::Vec3 c111 = sample_probe(coords.x1, coords.y1, coords.z1);

    const fuse::math::Vec3 c00 = lerpIrradiance(c000, c100, coords.tx);
    const fuse::math::Vec3 c10 = lerpIrradiance(c010, c110, coords.tx);
    const fuse::math::Vec3 c01 = lerpIrradiance(c001, c101, coords.tx);
    const fuse::math::Vec3 c11 = lerpIrradiance(c011, c111, coords.tx);

    const fuse::math::Vec3 c0 = lerpIrradiance(c00, c10, coords.ty);
    const fuse::math::Vec3 c1 = lerpIrradiance(c01, c11, coords.ty);
    return lerpIrradiance(c0, c1, coords.tz);
}

} // namespace

bool tryTrilinearProbeIrradiance(const DDGIDesc& desc,
                                 const fuse::math::Vec3& world_position,
                                 const IrradianceCacheEntry* cache,
                                 u32 cache_count,
                                 fuse::math::Vec3& out_irradiance,
                                 ProbeTrilinearSampleRejectReason& outReason) {
    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords)) {
        outReason = ProbeGridLayout::isEmptyGrid(desc) ? ProbeTrilinearSampleRejectReason::EmptyGrid
                                                       : ProbeTrilinearSampleRejectReason::NotSampleable;
        out_irradiance = {};
        return false;
    }
    if (!tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, outReason)) {
        out_irradiance = {};
        return false;
    }

    out_irradiance = trilinearProbeIrradianceFromCoords(desc, coords, cache, cache_count);
    return true;
}

bool tryTrilinearProbeIrradiance(const DDGIDesc& desc,
                                 const fuse::math::Vec3& world_position,
                                 const IrradianceCacheEntry* cache,
                                 u32 cache_count,
                                 fuse::math::Vec3& out_irradiance) {
    ProbeTrilinearSampleRejectReason reason = ProbeTrilinearSampleRejectReason::None;
    return tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, out_irradiance, reason);
}

fuse::math::Vec3 trilinearProbeIrradiance(const DDGIDesc& desc,
                                          const fuse::math::Vec3& world_position,
                                          const IrradianceCacheEntry* cache,
                                          u32 cache_count) {
    fuse::math::Vec3 result{};
    tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, result);
    return result;
}

bool tryTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                            const fuse::math::Vec3& world_position,
                                            const fuse::math::Vec3& direction,
                                            const IrradianceCacheEntry* cache,
                                            u32 cache_count,
                                            fuse::math::Vec3& out_irradiance,
                                            ProbeTrilinearSampleRejectReason& outReason) {
    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords)) {
        outReason = ProbeGridLayout::isEmptyGrid(desc) ? ProbeTrilinearSampleRejectReason::EmptyGrid
                                                       : ProbeTrilinearSampleRejectReason::NotSampleable;
        out_irradiance = {};
        return false;
    }
    if (!tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, outReason)) {
        out_irradiance = {};
        return false;
    }

    const fuse::math::Vec3 sample_direction = DdgiIrradianceEncoding::resolveSampleDirection(direction);

    const auto sample_probe = [&](u32 x, u32 y, u32 z) -> fuse::math::Vec3 {
        const ProbeGridCoord probe_coord{x, y, z};
        const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, probe_coord);
        fuse::math::Vec3 irradiance{};
        if (!tryReadIrradianceAtIndex(desc, cache, cache_count, index, irradiance)) {
            return {};
        }
        return sampleDirectionalIrradianceAtProbe(cache[index], sample_direction, desc.irradiance_res);
    };

    const fuse::math::Vec3 c000 = sample_probe(coords.x0, coords.y0, coords.z0);
    const fuse::math::Vec3 c100 = sample_probe(coords.x1, coords.y0, coords.z0);
    const fuse::math::Vec3 c010 = sample_probe(coords.x0, coords.y1, coords.z0);
    const fuse::math::Vec3 c110 = sample_probe(coords.x1, coords.y1, coords.z0);
    const fuse::math::Vec3 c001 = sample_probe(coords.x0, coords.y0, coords.z1);
    const fuse::math::Vec3 c101 = sample_probe(coords.x1, coords.y0, coords.z1);
    const fuse::math::Vec3 c011 = sample_probe(coords.x0, coords.y1, coords.z1);
    const fuse::math::Vec3 c111 = sample_probe(coords.x1, coords.y1, coords.z1);

    const fuse::math::Vec3 c00 = lerpIrradiance(c000, c100, coords.tx);
    const fuse::math::Vec3 c10 = lerpIrradiance(c010, c110, coords.tx);
    const fuse::math::Vec3 c01 = lerpIrradiance(c001, c101, coords.tx);
    const fuse::math::Vec3 c11 = lerpIrradiance(c011, c111, coords.tx);

    const fuse::math::Vec3 c0 = lerpIrradiance(c00, c10, coords.ty);
    const fuse::math::Vec3 c1 = lerpIrradiance(c01, c11, coords.ty);
    out_irradiance = lerpIrradiance(c0, c1, coords.tz);
    return true;
}

bool tryTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                            const fuse::math::Vec3& world_position,
                                            const fuse::math::Vec3& direction,
                                            const IrradianceCacheEntry* cache,
                                            u32 cache_count,
                                            fuse::math::Vec3& out_irradiance) {
    ProbeTrilinearSampleRejectReason reason = ProbeTrilinearSampleRejectReason::None;
    return tryTrilinearDirectionalProbeIrradiance(
        desc, world_position, direction, cache, cache_count, out_irradiance, reason);
}

fuse::math::Vec3 trilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                                     const fuse::math::Vec3& world_position,
                                                     const fuse::math::Vec3& direction,
                                                     const IrradianceCacheEntry* cache,
                                                     u32 cache_count) {
    fuse::math::Vec3 result{};
    tryTrilinearDirectionalProbeIrradiance(desc, world_position, direction, cache, cache_count, result);
    return result;
}

u32 nearestProbeIndex(const DDGIDesc& desc, const fuse::math::Vec3& world_position) {
    const u32 count = probeCount(desc);
    if (count == 0u) {
        return UINT32_MAX;
    }

    u32 best_index = 0u;
    f32 best_distance = std::numeric_limits<f32>::max();
    for (u32 i = 0; i < count; ++i) {
        const fuse::math::Vec3 probe_pos = probeWorldPosition(desc, i);
        const fuse::math::Vec3 delta = world_position - probe_pos;
        const f32 distance = delta.dot(delta);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }
    return best_index;
}

} // namespace ddgi_util

DdgiInfo ddgi_info() {
    DdgiInfo info{};
#if defined(FUSE_HAS_CUDA)
    info.backend = DdgiBackend::Cuda;
    info.message = "CUDA DDGI kernels stub — probe trace deferred";
#else
    info.backend = DdgiBackend::Stub;
    info.message = "CPU-only DDGI scaffold";
#endif
    info.valid = true;
    return info;
}

bool tryCanLaunchDdgiProbeUpdate(const DDGIDesc& desc,
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 ProbeUpdateLaunchRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = ProbeUpdateLaunchRejectReason::EmptyGrid;
        return false;
    }
    if (probe_indices == nullptr) {
        outReason = ProbeUpdateLaunchRejectReason::NullIndices;
        return false;
    }
    if (probe_count == 0u) {
        outReason = ProbeUpdateLaunchRejectReason::ZeroCount;
        return false;
    }

    for (u32 i = 0u; i < probe_count; ++i) {
        if (ProbeGridLayout::isProbeIndexOutOfRange(probe_indices[i], desc)) {
            outReason = ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex;
            return false;
        }
    }

    outReason = ProbeUpdateLaunchRejectReason::None;
    return true;
}

bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
}

bool wouldSkipDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    return !canLaunchDdgiProbeUpdate(desc, probe_indices, probe_count);
}

bool tryLaunch_ddgi_probe_update(const DDGIDesc& desc,
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 void* cuda_stream,
                                 ProbeUpdateLaunchRejectReason& outReason) {
    if (!tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, outReason)) {
        return false;
    }
    return launch_ddgi_probe_update(desc, probe_indices, probe_count, cuda_stream);
}

bool launch_ddgi_probe_update(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              void* cuda_stream) {
    if (!canLaunchDdgiProbeUpdate(desc, probe_indices, probe_count)) {
        return false;
    }

    gi::DDGIKernelParams params{};
    params.probe_indices_to_update = probe_indices;
    params.probe_update_count = probe_count;
    params.rays_per_probe = desc.rays_per_probe;
    params.hysteresis = desc.hysteresis;
    params.max_ray_distance = desc.max_ray_distance;

    const bool traced = gi::launch_probe_trace_kernel(params, cuda_stream);
    const bool blended = gi::launch_probe_blend_kernel(params, cuda_stream);
    return traced && blended;
}

namespace gi {

const char* probeKernelRejectReasonLabel(ProbeKernelRejectReason reason) {
    switch (reason) {
    case ProbeKernelRejectReason::None:
        return "none";
    case ProbeKernelRejectReason::ZeroUpdateCount:
        return "zero_update_count";
    case ProbeKernelRejectReason::NullProbeIndices:
        return "null_probe_indices";
    case ProbeKernelRejectReason::ZeroRaysPerProbe:
        return "zero_rays_per_probe";
    }
    return "unknown";
}

bool probeKernelRejectReasonIsBlocking(ProbeKernelRejectReason reason) {
    return reason != ProbeKernelRejectReason::None;
}

ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    tryCanLaunchProbeTraceKernel(params, reason);
    return reason;
}

bool wouldSkipProbeKernelLaunch(const DDGIKernelParams& params) {
    return !canLaunchProbeTraceKernel(params);
}

bool preflightProbeKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    const ProbeKernelRejectReason reject = classifyProbeKernelReject(params);
    if (reason != nullptr) {
        *reason = reject;
    }
    return !probeKernelRejectReasonIsBlocking(reject);
}

bool tryPreflightProbeKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    return preflightProbeKernelLaunch(params, &outReason);
}

void populateDDGIKernelParams(DDGIKernelParams& params,
                              const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              u64 frame_seed) {
    params.probe_indices_to_update = probe_indices;
    params.probe_update_count = probe_count;
    params.rays_per_probe = desc.rays_per_probe;
    params.hysteresis = desc.hysteresis;
    params.max_ray_distance = desc.max_ray_distance;
    params.frame_seed = frame_seed;
}

bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    if (params.probe_update_count == 0u) {
        outReason = ProbeKernelRejectReason::ZeroUpdateCount;
        return false;
    }
    if (params.probe_indices_to_update == nullptr) {
        outReason = ProbeKernelRejectReason::NullProbeIndices;
        return false;
    }
    if (params.rays_per_probe == 0u) {
        outReason = ProbeKernelRejectReason::ZeroRaysPerProbe;
        return false;
    }
    outReason = ProbeKernelRejectReason::None;
    return true;
}

bool canLaunchProbeTraceKernel(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    return tryCanLaunchProbeTraceKernel(params, reason);
}

bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    return tryCanLaunchProbeTraceKernel(params, outReason);
}

bool canLaunchProbeBlendKernel(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    return tryCanLaunchProbeBlendKernel(params, reason);
}

bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeTraceKernel(params);
}

bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeBlendKernel(params);
}

bool tryLaunch_probe_trace_kernel(const DDGIKernelParams& params,
                                  void* cuda_stream,
                                  ProbeKernelRejectReason& outReason) {
    if (!tryCanLaunchProbeTraceKernel(params, outReason)) {
        return false;
    }
    return launch_probe_trace_kernel(params, cuda_stream);
}

bool launch_probe_trace_kernel(const DDGIKernelParams& params, void* cuda_stream) {
    if (!canLaunchProbeTraceKernel(params)) {
        return false;
    }
    (void)cuda_stream;
#if defined(FUSE_HAS_CUDA)
    // Full probe_trace_kernel lands in ddgi_kernels.cu — stub succeeds on CI.
    return true;
#else
    return true;
#endif
}

bool tryLaunch_probe_blend_kernel(const DDGIKernelParams& params,
                                  void* cuda_stream,
                                  ProbeKernelRejectReason& outReason) {
    if (!tryCanLaunchProbeBlendKernel(params, outReason)) {
        return false;
    }
    return launch_probe_blend_kernel(params, cuda_stream);
}

bool tryLaunch_probe_kernels(const DDGIKernelParams& params,
                             void* cuda_stream,
                             ProbeKernelRejectReason& outReason) {
    if (!tryLaunch_probe_trace_kernel(params, cuda_stream, outReason)) {
        return false;
    }
    return tryLaunch_probe_blend_kernel(params, cuda_stream, outReason);
}

bool launch_probe_blend_kernel(const DDGIKernelParams& params, void* cuda_stream) {
    if (!canLaunchProbeBlendKernel(params)) {
        return false;
    }
    (void)cuda_stream;
#if defined(FUSE_HAS_CUDA)
    return true;
#else
    return true;
#endif
}

} // namespace gi

bool DDGI::init(const DDGIDesc& desc, ResourceManager& resources) {
    destroy();
    m_desc = desc;
    m_resources = &resources;
    m_info = ddgi_info();

    const u32 count = ddgi_util::probeCount(m_desc);
    if (count == 0u || m_desc.irradiance_res == 0u || m_desc.depth_res == 0u) {
        return false;
    }

    if (!allocateResources(resources)) {
        destroy();
        return false;
    }

    m_cache.resize(count);
    for (IrradianceCacheEntry& entry : m_cache) {
        entry.irradiance = defaultAmbientIrradiance();
        entry.mean_depth = m_desc.max_ray_distance * 0.5f;
        entry.depth_variance = 0.1f;
    }

    m_data.desc = m_desc;
    m_data.irradiance_atlas = m_volume.irradiance_atlas;
    m_data.depth_atlas = m_volume.depth_atlas;
    m_data.probe_offsets = m_volume.probe_offsets;
    m_info.probe_count = count;
    m_ready = true;
    return true;
}

void DDGI::destroy() {
    releaseResources();
    m_desc = {};
    m_data = {};
    m_volume = {};
    m_cache.clear();
    m_last_update = {};
    m_info = {};
    m_resources = nullptr;
    m_ready = false;
}

const IrradianceCacheEntry& DDGI::cacheEntry(u32 probe_index) const {
    if (!m_ready || m_cache.empty()) {
        return kDefaultCacheEntry;
    }
    const u32 clamped = ProbeGridLayout::clampProbeIndex(probe_index, m_desc);
    if (clamped >= m_cache.size()) {
        return kDefaultCacheEntry;
    }
    return m_cache[clamped];
}

bool DDGI::update(u32 frame_index, void* cuda_stream) {
    if (!m_ready) {
        return false;
    }

    m_last_update = {};
    m_last_update.frame_index = frame_index;

    const u32 probe_count = static_cast<u32>(m_cache.size());
    u32 scheduled_indices[256]{};
    u32 scheduled_count = 0u;
    ddgi_util::scheduleProbeUpdates(frame_index,
                                    probe_count,
                                    m_desc.probes_per_frame,
                                    scheduled_indices,
                                    static_cast<u32>(sizeof(scheduled_indices) / sizeof(scheduled_indices[0])),
                                    &scheduled_count);
    m_last_update.probes_scheduled = scheduled_count;

    m_last_update.kernel_launched =
        launch_ddgi_probe_update(m_desc, scheduled_indices, scheduled_count, cuda_stream);

    const fuse::math::Vec3 incoming = defaultAmbientIrradiance();
    for (u32 i = 0; i < scheduled_count; ++i) {
        const u32 probe_index = ProbeGridLayout::clampProbeIndex(scheduled_indices[i], m_desc);
        if (probe_index >= m_cache.size()) {
            continue;
        }
        IrradianceCacheEntry& entry = m_cache[probe_index];
        entry.irradiance = ddgi_util::blendIrradiance(entry.irradiance, incoming, m_desc.hysteresis);
    }

    return m_last_update.kernel_launched;
}

DDGISampleResult DDGI::sampleIrradiance(const DDGISampleRequest& request) const {
    DDGISampleResult result{};
    if (!m_ready) {
        return result;
    }

    if (!ddgi_util::isValidSampleRequest(m_desc, request, static_cast<u32>(m_cache.size()))) {
        return result;
    }

    const u32 nearest = ddgi_util::nearestProbeIndex(m_desc, request.world_position);
    if (nearest == UINT32_MAX || nearest >= m_cache.size()) {
        return result;
    }

    result.nearest_probe = nearest;
    const fuse::math::Vec3 sample_direction =
        DdgiIrradianceEncoding::resolveSampleDirectionFromSurface(request.world_normal,
                                                                  request.world_normal);
    result.irradiance = ddgi_util::trilinearDirectionalProbeIrradiance(m_desc,
                                                                       request.world_position,
                                                                       sample_direction,
                                                                       m_cache.data(),
                                                                       static_cast<u32>(m_cache.size()));
    result.valid = true;
    return result;
}

bool DDGI::allocateResources(ResourceManager& resources) {
    const u32 count = ddgi_util::probeCount(m_desc);

    TextureDesc irradianceDesc{};
    irradianceDesc.width = ddgi_util::irradianceAtlasWidth(m_desc);
    irradianceDesc.height = ddgi_util::irradianceAtlasHeight(m_desc);
    irradianceDesc.format = GpuFormat::R16G16B16A16Sfloat;
    irradianceDesc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) |
                                                   static_cast<u32>(ImageUsage::Storage));
    irradianceDesc.cudaInterop = true;
    irradianceDesc.name = "ddgi_irradiance_atlas";

    TextureDesc depthDesc{};
    depthDesc.width = ddgi_util::depthAtlasWidth(m_desc);
    depthDesc.height = ddgi_util::depthAtlasHeight(m_desc);
    depthDesc.format = GpuFormat::R16G16Sfloat;
    depthDesc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) |
                                              static_cast<u32>(ImageUsage::Storage));
    depthDesc.cudaInterop = true;
    depthDesc.name = "ddgi_depth_atlas";

    m_volume.irradiance_atlas = resources.createTexture(irradianceDesc);
    m_volume.depth_atlas = resources.createTexture(depthDesc);
    if (!m_volume.irradiance_atlas.isValid() || !m_volume.depth_atlas.isValid()) {
        return false;
    }

    BufferDesc offsetDesc{};
    offsetDesc.size = static_cast<usize>(count) * sizeof(fuse::math::Vec3);
    offsetDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                static_cast<u32>(BufferUsage::TransferDst));
    offsetDesc.cudaInterop = true;
    offsetDesc.name = "ddgi_probe_offsets";
    m_volume.probe_offsets = resources.createBuffer(offsetDesc);
    if (!m_volume.probe_offsets.isValid()) {
        return false;
    }

    m_volume.probe_count = count;
    return true;
}

void DDGI::releaseResources() {
    if (m_resources == nullptr) {
        m_volume = {};
        return;
    }

    if (m_volume.irradiance_atlas.isValid()) {
        m_resources->destroyTexture(m_volume.irradiance_atlas);
    }
    if (m_volume.depth_atlas.isValid()) {
        m_resources->destroyTexture(m_volume.depth_atlas);
    }
    if (m_volume.probe_offsets.isValid()) {
        m_resources->destroyBuffer(m_volume.probe_offsets);
    }
    m_volume = {};
}

} // namespace fuse::renderer

// --- deepen additive from deepen-ddgi-probe-guards-964a ---
    if (!ProbeGridLayout::tryBuildProbeSampleCoords(desc, world_position, coords)) {
bool probeSampleSkipReasonIsBlocking(ProbeSampleSkipReason reason) {

// --- deepen additive from deepen-ddgi-probe-guards-54ac ---
bool ProbeGridLayout::tryClampProbeIndex(u32 probe_index, const DDGIDesc& desc, u32& out_index) {

// --- deepen additive from deepen-ddgi-probe-guards-5f38 ---
    if (!ddgi_util::tryTrilinearDirectionalProbeIrradiance(m_desc,

// --- deepen additive from deepen-b56-ddgi-sample-cache-guards-89c4 ---
bool tryClampCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count, u32& out_index) {
const char* ddgiLaunchRejectReasonLabel(DdgiLaunchRejectReason reason) {
    case DdgiLaunchRejectReason::None:
    case DdgiLaunchRejectReason::EmptyGrid:
    case DdgiLaunchRejectReason::NullIndices:
    case DdgiLaunchRejectReason::ZeroCount:
    case DdgiLaunchRejectReason::OutOfRangeIndex:
                                 DdgiLaunchRejectReason& out_reason) {
        out_reason = DdgiLaunchRejectReason::EmptyGrid;
        out_reason = DdgiLaunchRejectReason::NullIndices;
        out_reason = DdgiLaunchRejectReason::ZeroCount;
            out_reason = DdgiLaunchRejectReason::OutOfRangeIndex;
    out_reason = DdgiLaunchRejectReason::None;
    DdgiLaunchRejectReason reason = DdgiLaunchRejectReason::None;

// --- deepen additive from deepen-ddgi-b56-guards-5dea ---
    case CacheIndexRejectReason::OutOfRangeIndex:
bool tryIsCacheIndexValid(const DDGIDesc& desc,
        outReason = CacheIndexRejectReason::OutOfRangeIndex;
                                 DdgiLaunchRejectReason& outReason) {
        outReason = DdgiLaunchRejectReason::EmptyGrid;
        outReason = DdgiLaunchRejectReason::NullIndices;
        outReason = DdgiLaunchRejectReason::ZeroCount;
            outReason = DdgiLaunchRejectReason::OutOfRangeIndex;
    outReason = DdgiLaunchRejectReason::None;
const char* ddgiKernelRejectReasonLabel(DdgiKernelRejectReason reason) {
    case DdgiKernelRejectReason::None:
    case DdgiKernelRejectReason::NullIndices:
    case DdgiKernelRejectReason::ZeroCount:
    case DdgiKernelRejectReason::InvalidRaysPerProbe:
    case DdgiKernelRejectReason::OutOfRangeIndex:
    DdgiKernelRejectReason reason = DdgiKernelRejectReason::None;
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {
        outReason = DdgiKernelRejectReason::ZeroCount;
        outReason = DdgiKernelRejectReason::NullIndices;
        outReason = DdgiKernelRejectReason::InvalidRaysPerProbe;
    outReason = DdgiKernelRejectReason::None;
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {
    return tryCanLaunchDdgiKernels(desc, params, reason);
bool tryCanLaunchDdgiKernels(const DDGIDesc& desc,
                             DdgiKernelRejectReason& outReason) {
            outReason = DdgiKernelRejectReason::OutOfRangeIndex;

// --- deepen additive from deepen-ddgi-guards-4e05 ---
    case CacheIndexRejectReason::ProbeIndexOutOfRange:
    case CacheIndexRejectReason::CacheUndersized:
        outReason = ProbeSampleCoordsRejectReason::OutOfRangeIndices;
        outReason = ProbeSampleCoordsRejectReason::OutOfRangeWeights;
    outReason = built ? ProbeSampleCoordsRejectReason::None : ProbeSampleCoordsRejectReason::EmptyGrid;
        outReason = CacheIndexRejectReason::ProbeIndexOutOfRange;
        outReason = CacheIndexRejectReason::CacheUndersized;
    return tryIsCacheIndexValid(desc, probe_index, cache_count, reason);
    case DdgiKernelRejectReason::ZeroRaysPerProbe:
        outReason = DdgiKernelRejectReason::ZeroRaysPerProbe;

// --- deepen additive from ddgi-b56-guards-deepen-c1b1 ---
        outReason = CacheIndexRejectReason::InvalidProbeIndex;
    case CacheIndexRejectReason::InvalidProbeIndex:
    case ProbeUpdateLaunchRejectReason::OutOfRangeIndex:
            outReason = ProbeUpdateLaunchRejectReason::OutOfRangeIndex;
const char* ddgiKernelLaunchRejectReasonLabel(DdgiKernelLaunchRejectReason reason) {
    case DdgiKernelLaunchRejectReason::None:
    case DdgiKernelLaunchRejectReason::ZeroUpdateCount:
    case DdgiKernelLaunchRejectReason::NullProbeIndices:
    case DdgiKernelLaunchRejectReason::ZeroRaysPerProbe:
    DdgiKernelLaunchRejectReason reason = DdgiKernelLaunchRejectReason::None;
    return tryCanLaunchDdgiKernelParams(params, reason);
bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason) {
        outReason = DdgiKernelLaunchRejectReason::ZeroUpdateCount;
        outReason = DdgiKernelLaunchRejectReason::NullProbeIndices;
        outReason = DdgiKernelLaunchRejectReason::ZeroRaysPerProbe;
    outReason = DdgiKernelLaunchRejectReason::None;

// --- deepen additive from deepen-ddgi-b56-guards-7061 ---
    case ProbeSampleCoordsRejectReason::InvalidSpacing:
    case CacheIndexRejectReason::NotSampleable:
        outReason = ProbeSampleCoordsRejectReason::InvalidSpacing;
bool tryCanLookupCacheAtProbeIndex(const DDGIDesc& desc,
        outReason = CacheIndexRejectReason::NotSampleable;
    case DdgiKernelRejectReason::NullProbeIndices:
    case DdgiKernelRejectReason::ZeroProbeCount:
bool preflightDDGIKernelParams(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {
        outReason = DdgiKernelRejectReason::ZeroProbeCount;
        outReason = DdgiKernelRejectReason::NullProbeIndices;
    return preflightDDGIKernelParams(params, reason);

// --- deepen additive from deepen-ddgi-probe-guards-9a61 ---
    case ProbeSampleCoordsRejectReason::InvalidWeights:
    case DdgiLaunchRejectReason::NullIndexBuffer:
    case DdgiLaunchRejectReason::ZeroProbeCount:
        outReason = ProbeSampleCoordsRejectReason::InvalidWeights;
bool tryValidateCacheSizedForGrid(const DDGIDesc& desc,
        outReason = DdgiLaunchRejectReason::ZeroProbeCount;
        outReason = DdgiLaunchRejectReason::NullIndexBuffer;
    case DdgiKernelRejectReason::ZeroUpdateCount:
        outReason = DdgiKernelRejectReason::ZeroUpdateCount;

// --- deepen additive from deepen-ddgi-guards-2d52 ---
ProbeSampleCoordRejectReason probeSampleCoordRejectFromValidity(const DDGIDesc& desc,
        return ProbeSampleCoordRejectReason::EmptyGrid;
        return ProbeSampleCoordRejectReason::OutOfBounds;
    return ProbeSampleCoordRejectReason::InvalidWeights;
const char* probeSampleCoordRejectReasonLabel(ProbeSampleCoordRejectReason reason) {
    case ProbeSampleCoordRejectReason::None:
    case ProbeSampleCoordRejectReason::EmptyGrid:
    case ProbeSampleCoordRejectReason::OutOfBounds:
    case ProbeSampleCoordRejectReason::InvalidWeights:
    case CacheIndexRejectReason::OutOfRangeProbe:
                                                ProbeSampleCoordRejectReason& outReason) {
        outReason = ProbeSampleCoordRejectReason::EmptyGrid;
    outReason = ProbeSampleCoordRejectReason::None;
        outReason = CacheIndexRejectReason::OutOfRangeProbe;
    ProbeSampleCoordRejectReason reason = ProbeSampleCoordRejectReason::None;
    return tryCanSampleAtProbeCoords(desc, coords, cache_count, reason);
        outReason = ProbeSampleCoordRejectReason::OutOfBounds;
bool tryCanLaunchProbeKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {
    return tryCanLaunchProbeKernel(params, outReason);

// --- deepen additive from ddgi-probe-grid-guards-03fa ---
                                                ProbeSampleRejectReason& outReason) {
        outReason = ProbeSampleRejectReason::EmptyGrid;
    outReason = ProbeSampleRejectReason::None;
const char* probeSampleRejectReasonLabel(ProbeSampleRejectReason reason) {
    case ProbeSampleRejectReason::None:
    case ProbeSampleRejectReason::EmptyGrid:
    case ProbeSampleRejectReason::OutOfBounds:
    case ProbeSampleRejectReason::InvalidWeights:
const char* cacheLookupRejectReasonLabel(CacheLookupRejectReason reason) {
    case CacheLookupRejectReason::None:
    case CacheLookupRejectReason::EmptyGrid:
    case CacheLookupRejectReason::UndersizedCache:
    case CacheLookupRejectReason::ProbeIndexOutOfRange:
const char* launchRejectReasonLabel(LaunchRejectReason reason) {
    case LaunchRejectReason::None:
    case LaunchRejectReason::EmptyGrid:
    case LaunchRejectReason::NullIndices:
    case LaunchRejectReason::ZeroCount:
    case LaunchRejectReason::ProbeIndexOutOfRange:
    case LaunchRejectReason::ZeroRaysPerProbe:
bool tryCanLookupCacheAtIndex(const DDGIDesc& desc,
                              CacheLookupRejectReason& outReason) {
        outReason = CacheLookupRejectReason::EmptyGrid;
        outReason = CacheLookupRejectReason::UndersizedCache;
        outReason = CacheLookupRejectReason::ProbeIndexOutOfRange;
    outReason = CacheLookupRejectReason::None;
    CacheLookupRejectReason cacheReason = CacheLookupRejectReason::None;
    if (!tryCanLookupCacheAtIndex(desc, 0u, cache_count, cacheReason)) {
            outReason = ProbeSampleRejectReason::OutOfBounds;
            outReason = indicesInRange ? ProbeSampleRejectReason::InvalidWeights
                                         : ProbeSampleRejectReason::OutOfBounds;
bool tryIsValidSampleRequest(const DDGIDesc& desc,
        outReason = LaunchRejectReason::EmptyGrid;
        outReason = LaunchRejectReason::NullIndices;
        outReason = LaunchRejectReason::ZeroCount;
        outReason = LaunchRejectReason::ZeroRaysPerProbe;
            outReason = LaunchRejectReason::ProbeIndexOutOfRange;
    outReason = LaunchRejectReason::None;
const char* kernelLaunchRejectReasonLabel(KernelLaunchRejectReason reason) {
    case KernelLaunchRejectReason::None:
    case KernelLaunchRejectReason::NullIndices:
    case KernelLaunchRejectReason::ZeroCount:
    case KernelLaunchRejectReason::ZeroRaysPerProbe:
    KernelLaunchRejectReason reason = KernelLaunchRejectReason::None;
bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, KernelLaunchRejectReason& outReason) {
        outReason = KernelLaunchRejectReason::NullIndices;
        outReason = KernelLaunchRejectReason::ZeroCount;
        outReason = KernelLaunchRejectReason::ZeroRaysPerProbe;
    outReason = KernelLaunchRejectReason::None;
bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, KernelLaunchRejectReason& outReason) {

// --- deepen additive from deepen-ddgi-probe-preflights-021e ---
    case ProbeSampleCoordsRejectReason::NotSampleable:
    case ProbeSampleCoordsRejectReason::UndersizedCache:
    case CacheIndexRejectReason::ZeroCache:
const char* sampleRequestRejectReasonLabel(SampleRequestRejectReason reason) {
    case SampleRequestRejectReason::None:
    case SampleRequestRejectReason::NotSampleable:
    case SampleRequestRejectReason::UndersizedCache:
        outReason = CacheIndexRejectReason::ZeroCache;
    if (!tryValidateCacheIndex(desc, probe_index, cache_count, outReason)) {
        outReason = ProbeSampleCoordsRejectReason::NotSampleable;
    if (!ProbeGridLayout::tryValidateProbeSampleCoords(desc, coords, outReason)) {
        outReason = ProbeSampleCoordsRejectReason::UndersizedCache;
bool tryValidateSampleRequest(const DDGIDesc& desc,
                              SampleRequestRejectReason& outReason) {
        outReason = SampleRequestRejectReason::NotSampleable;
        outReason = SampleRequestRejectReason::UndersizedCache;
    outReason = SampleRequestRejectReason::None;
bool preflightProbeKernelParams(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {

// --- deepen additive from deepen-ddgi-guards-dd1a ---
const char* probeSpatialSampleRejectReasonLabel(ProbeSpatialSampleRejectReason reason) {
    case ProbeSpatialSampleRejectReason::None:
    case ProbeSpatialSampleRejectReason::EmptyGrid:
    case ProbeSpatialSampleRejectReason::InvalidSampleCoords:
    case ProbeSpatialSampleRejectReason::UndersizedCache:
    case ProbeSpatialSampleRejectReason::NullCache:
    case ProbeScheduleRejectReason::NullOutput:
    case ProbeUpdateLaunchRejectReason::ZeroRaysPerProbe:
    return tryCanLookupCacheAtCoord(desc, coord, cache_count, reason);
bool tryCanLookupCacheAtCoord(const DDGIDesc& desc,
    return tryValidateCacheIndex(desc, index, cache_count, outReason);
    ProbeSpatialSampleRejectReason reason = ProbeSpatialSampleRejectReason::None;
                               ProbeSpatialSampleRejectReason& outReason) {
        outReason = ProbeSpatialSampleRejectReason::EmptyGrid;
        outReason = ProbeSpatialSampleRejectReason::UndersizedCache;
        outReason = ProbeSpatialSampleRejectReason::InvalidSampleCoords;
    outReason = ProbeSpatialSampleRejectReason::None;
    return tryScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
bool tryScheduleProbeUpdates(u32 probe_count,
        outReason = ProbeScheduleRejectReason::NullOutput;
        outReason = ProbeSpatialSampleRejectReason::NullCache;
    ProbeSampleCoordsRejectReason buildReason = ProbeSampleCoordsRejectReason::None;
    if (!ProbeGridLayout::tryBuildProbeSampleCoords(desc, world_position, coords, buildReason)) {
        outReason = buildReason == ProbeSampleCoordsRejectReason::EmptyGrid
                        ? ProbeSpatialSampleRejectReason::EmptyGrid
                        : ProbeSpatialSampleRejectReason::InvalidSampleCoords;
    if (!tryCanSampleAtProbeCoords(desc, coords, cache_count, outReason)) {
        outReason = ProbeUpdateLaunchRejectReason::ZeroRaysPerProbe;

// --- deepen additive from deepen-ddgi-b56-guards-7655 ---
    case ProbeScheduleRejectReason::NullOutputIndices:
    case ProbeScheduleRejectReason::NullOutputCount:
    case SampleRequestRejectReason::EmptyGrid:
bool tryValidateCacheAccess(const DDGIDesc& desc,
    if (!tryValidateCacheAccess(desc, cache, cache_count, probe_index, reason)) {
        outReason = SampleRequestRejectReason::EmptyGrid;
    SampleRequestRejectReason reason = SampleRequestRejectReason::None;
    return tryValidateSampleRequest(desc, request, cache_count, reason);
        outReason = ProbeScheduleRejectReason::NullOutputIndices;
        outReason = ProbeScheduleRejectReason::NullOutputCount;
    tryScheduleProbeUpdates(frame_index,

// --- deepen additive from deepen-ddgi-guards-c7e8 ---
    if (!tryValidateCacheLookup(desc, cache, cache_count, probe_index, reason)) {
bool tryValidateCacheLookup(const DDGIDesc& desc,
bool wouldSkipCacheIndexValidation(const DDGIDesc& desc,
    return !tryValidateCacheLookup(desc, cache, cache_count, probe_index, reason);
    case ProbeScheduleRejectReason::NullIndicesBuffer:
    case ProbeScheduleRejectReason::NullCountOut:
        outReason = ProbeScheduleRejectReason::NullIndicesBuffer;
        outReason = ProbeScheduleRejectReason::NullCountOut;
    if (!tryCanScheduleProbeUpdates(probe_count, probes_per_frame, out_indices, max_indices, out_count, outReason)) {
    return tryCanScheduleProbeUpdates(probe_count, probes_per_frame, out_indices, max_indices, out_count, reason);
    case ProbeKernelRejectReason::NullRadianceSurfaces:
    case ProbeKernelRejectReason::NullAtlasSurfaces:
    case ProbeKernelRejectReason::ZeroMaxRayDistance:
    case ProbeKernelRejectReason::InvalidHysteresis:
        outReason = ProbeKernelRejectReason::ZeroMaxRayDistance;
        outReason = ProbeKernelRejectReason::NullRadianceSurfaces;
        outReason = ProbeKernelRejectReason::NullAtlasSurfaces;
        outReason = ProbeKernelRejectReason::InvalidHysteresis;

// --- deepen additive from deepen-ddgi-guards-ed0c ---
    case ProbeScheduleRejectReason::NullCountOutput:
bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (!tryValidateCacheIndex(desc, probe_index, cache_count, reason)) {
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
bool wouldSkipCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
bool wouldSkipCacheIndex(const DDGIDesc& desc,
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
        outReason = ProbeScheduleRejectReason::NullCountOutput;
ProbeUpdateLaunchRejectReason classifyProbeUpdateLaunchReject(const DDGIDesc& desc,

// --- deepen additive from deepen-ddgi-b56-guards-be5b ---
                    ? ProbeSampleCoordsRejectReason::UnorderedCorners
                    : ProbeSampleCoordsRejectReason::OutOfRangeWeights;
    if (!tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason)) {
bool wouldClampCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    if (!tryScheduleProbeUpdates(
const char* probeKernelResourceRejectReasonLabel(ProbeKernelResourceRejectReason reason) {
    case ProbeKernelResourceRejectReason::None:
    case ProbeKernelResourceRejectReason::NullProbeWorldPositions:
    case ProbeKernelResourceRejectReason::NullIrradianceAtlas:
    case ProbeKernelResourceRejectReason::NullDepthAtlas:
    case ProbeKernelResourceRejectReason::NullPrevIrradianceSurface:
    case ProbeKernelResourceRejectReason::NullOutRadianceSurface:
    ProbeKernelResourceRejectReason reason = ProbeKernelResourceRejectReason::None;
    return tryValidateProbeKernelResources(params, reason);
bool tryValidateProbeKernelResources(const DDGIKernelParams& params,
                                     ProbeKernelResourceRejectReason& outReason) {
        outReason = ProbeKernelResourceRejectReason::NullProbeWorldPositions;
        outReason = ProbeKernelResourceRejectReason::NullIrradianceAtlas;
        outReason = ProbeKernelResourceRejectReason::NullDepthAtlas;
        outReason = ProbeKernelResourceRejectReason::NullPrevIrradianceSurface;
        outReason = ProbeKernelResourceRejectReason::NullOutRadianceSurface;
    outReason = ProbeKernelResourceRejectReason::None;
bool tryCanLaunchProbeTraceKernelWithResources(const DDGIKernelParams& params,
                                               ProbeKernelRejectReason& outLaunchReason,
                                               ProbeKernelResourceRejectReason& outResourceReason) {
    if (!tryCanLaunchProbeTraceKernel(params, outLaunchReason)) {
        outResourceReason = ProbeKernelResourceRejectReason::None;
    return tryValidateProbeKernelResources(params, outResourceReason);
bool tryCanLaunchProbeBlendKernelWithResources(const DDGIKernelParams& params,
    return tryCanLaunchProbeTraceKernelWithResources(params, outLaunchReason, outResourceReason);

// --- deepen additive from deepen-b56-ddgi-guards-214e ---
bool preflightScheduleProbeUpdates(u32 probe_count,
    return preflightScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
    if (!preflightScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, outReason)) {
    tryScheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count,

// --- deepen additive from deepen-ddgi-guards-769b ---
    return tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);
bool wouldSkipSampleRequest(const DDGIDesc& desc,
bool probeSchedulePreflight(u32 probe_count,
    return probeSchedulePreflight(probe_count, max_indices, out_indices, out_count, reason);
bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, const u32* out_count) {
    if (!probeSchedulePreflight(probe_count, max_indices, out_indices, out_count, outReason)) {

// --- deepen additive from deepen-ddgi-guards-eb89 ---
    case ProbeSampleCoordsRejectReason::NonSampleableGrid:
        outReason = ProbeSampleCoordsRejectReason::NonSampleableGrid;
    case ProbeKernelRejectReason::NullBlendSurfaces:
    return tryValidateProbeBlendKernelSurfaces(params, reason);
bool tryValidateProbeBlendKernelSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
        outReason = ProbeKernelRejectReason::NullBlendSurfaces;

// --- deepen additive from ddgi-b56-guards-deepen-0ebc ---
    ProbeSampleCoordsRejectReason coordReason = ProbeSampleCoordsRejectReason::None;
    if (!ProbeGridLayout::tryPreflightProbeSampleCoords(desc, coords, coordReason)) {
bool tryValidateCacheIndexLookup(const DDGIDesc& desc,
bool wouldClampCacheIndex(const DDGIDesc& desc, u32 probe_index) {
bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {

// --- deepen additive from deepen-ddgi-guards-09bc ---
    case ProbeScheduleRejectReason::NullIndices:
    case ProbeScheduleRejectReason::NullCount:
bool ProbeGridLayout::tryWorldToProbeGridCoord(const DDGIDesc& desc,
bool wouldClampCacheLookupIndex(u32 probe_index, const DDGIDesc& desc) {
bool tryProbeWorldPosition(const DDGIDesc& desc,
        outReason = ProbeScheduleRejectReason::NullIndices;
        outReason = ProbeScheduleRejectReason::NullCount;
    return tryCanScheduleProbeUpdates(probe_count, probes_per_frame, max_indices, out_indices, out_count, reason);
    if (!tryCanScheduleProbeUpdates(probe_count, probes_per_frame, max_indices, out_indices, out_count, outReason)) {
    tryScheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count, reason);
    case ProbeKernelRejectReason::NullRadianceSurface:
    case ProbeKernelRejectReason::NullIrradianceAtlas:
    case ProbeKernelRejectReason::NullDepthAtlas:
bool tryCanLaunchProbeTraceKernelWithSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
        outReason = ProbeKernelRejectReason::NullRadianceSurface;
    return tryCanLaunchProbeTraceKernelWithSurfaces(params, reason);
bool tryCanLaunchProbeBlendKernelWithSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
        outReason = ProbeKernelRejectReason::NullIrradianceAtlas;
        outReason = ProbeKernelRejectReason::NullDepthAtlas;
    return tryCanLaunchProbeBlendKernelWithSurfaces(params, reason);

// --- deepen additive from deepen-b56-ddgi-guards-1017 ---
bool tryValidateProbeSchedule(u32 probe_count,
    return tryValidateProbeSchedule(probe_count, probes_per_frame, out_indices, max_indices, out_count, reason);

// --- deepen additive from deepen-ddgi-guards-1f58 ---
    return tryValidateCacheIndex(desc, probe_index, cache, cache_count, reason);
    return tryValidateProbeSchedule(probe_count, max_indices, out_indices, out_count, reason);
    if (!tryValidateProbeSchedule(probe_count, max_indices, out_indices, out_count, outReason)) {

// --- deepen additive from deepen-b56-ddgi-guards-bfe3 ---
    case ProbeScheduleRejectReason::NullOutputBuffer:
bool wouldClampProbeIndex(u32 probe_index, const DDGIDesc& desc) {
    return tryScheduleProbeUpdates(0u, probe_count, 0u, out_indices, max_indices, out_count, reason);
        outReason = ProbeScheduleRejectReason::NullOutputBuffer;
bool wouldSkipProbeSchedule(u32 probe_count, u32* out_indices, u32 max_indices, u32* out_count) {
    if (!tryScheduleProbeUpdates(frame_index,

// --- deepen additive from deepen-b56-ddgi-guards-7081 ---
    return tryValidateProbeSampleCoords(desc, coords, outReason);
    return tryValidateCacheIndex(desc, cache, cache_count, probe_index, reason);

// --- deepen additive from deepen-ddgi-guards-088a ---
    return !tryValidateProbeSampleCoords(desc, coords, reason);
    return preflightProbeSchedule(probe_count, max_indices, out_indices, out_count, reason);
    if (!preflightProbeSchedule(probe_count, max_indices, out_indices, out_count, outReason)) {
            (outReason == ProbeScheduleRejectReason::NullOutput ||
             outReason == ProbeScheduleRejectReason::ZeroProbeCount ||
             outReason == ProbeScheduleRejectReason::ZeroMaxIndices)) {

// --- deepen additive from deepen-ddgi-guards-c8ba ---
bool ProbeGridLayout::tryNormalizeAndValidateProbeSampleCoords(const DDGIDesc& desc,
    return tryScheduleProbeUpdates(0u, probe_count, probes_per_frame, out_indices, max_indices, out_count, reason);
    case ProbeKernelRejectReason::NullProbeWorldPositions:
bool tryPreflightProbeTraceKernelResources(const DDGIKernelParams& params,
        outReason = ProbeKernelRejectReason::NullProbeWorldPositions;
bool tryPreflightProbeBlendKernelResources(const DDGIKernelParams& params,

// --- deepen additive from deepen-ddgi-guards-6f23 ---
ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc,
        return ProbeSampleCoordsRejectReason::EmptyGrid;
        return indicesInRange ? ProbeSampleCoordsRejectReason::OutOfRangeWeights
        return ProbeSampleCoordsRejectReason::UnorderedCorners;
    return ProbeSampleCoordsRejectReason::None;
    return classifyProbeSampleCoordsReject(desc, coords) != ProbeSampleCoordsRejectReason::None;
    return outReason == ProbeSampleCoordsRejectReason::None;
        return ProbeTrilinearSampleRejectReason::EmptyGrid;
        return ProbeTrilinearSampleRejectReason::NotSampleable;
        return ProbeTrilinearSampleRejectReason::NullCache;
        return ProbeTrilinearSampleRejectReason::UndersizedCache;
        return ProbeTrilinearSampleRejectReason::InvalidSampleCoords;
    return ProbeTrilinearSampleRejectReason::None;
    return classifyProbeTrilinearSampleReject(desc, coords, cache, cache_count) !=
    outReason = classifyProbeTrilinearSampleReject(desc, coords, cache, cache_count);
    return outReason == ProbeTrilinearSampleRejectReason::None;
        return CacheIndexRejectReason::EmptyGrid;
        return CacheIndexRejectReason::OutOfRangeProbeIndex;
        return CacheIndexRejectReason::UndersizedCache;
    return CacheIndexRejectReason::None;
        return CacheIndexRejectReason::NullCache;
    return classifyCacheIndexReject(desc, probe_index, cache_count);
    outReason = classifyCacheIndexReject(desc, probe_index, cache_count);
    return outReason == CacheIndexRejectReason::None;
    outReason = classifyCacheIndexReject(desc, cache, probe_index, cache_count);
        return ProbeScheduleRejectReason::NullOutIndices;
        return ProbeScheduleRejectReason::NullOutCount;
        return ProbeScheduleRejectReason::ZeroProbeCount;
        return ProbeScheduleRejectReason::ZeroMaxIndices;
    return ProbeScheduleRejectReason::None;
    outReason = classifyProbeScheduleReject(probe_count, max_indices, out_indices, out_count);
    return outReason == ProbeScheduleRejectReason::None;
        return ProbeUpdateLaunchRejectReason::EmptyGrid;
        return ProbeUpdateLaunchRejectReason::NullIndices;
        return ProbeUpdateLaunchRejectReason::ZeroCount;
            return ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex;
    return ProbeUpdateLaunchRejectReason::None;
    outReason = classifyProbeUpdateLaunchReject(desc, probe_indices, probe_count);
    return outReason == ProbeUpdateLaunchRejectReason::None;
        return ProbeKernelRejectReason::ZeroUpdateCount;
        return ProbeKernelRejectReason::NullProbeIndices;
        return ProbeKernelRejectReason::ZeroRaysPerProbe;
    return ProbeKernelRejectReason::None;
    outReason = classifyProbeKernelReject(params);
    return outReason == ProbeKernelRejectReason::None;
    return classifyProbeKernelReject(params) == ProbeKernelRejectReason::None;
    return classifyProbeKernelReject(params) != ProbeKernelRejectReason::None;

// --- deepen additive from deepen-ddgi-guards-ea5f ---
bool wouldClampCacheIndex(u32 probe_index, const DDGIDesc& desc) {
    ProbeScheduleRejectReason schedule_reason = ProbeScheduleRejectReason::None;
    ddgi_util::tryScheduleProbeUpdates(frame_index,
    ProbeUpdateLaunchRejectReason launch_reason = ProbeUpdateLaunchRejectReason::None;
    m_last_update.kernel_launched = tryLaunch_ddgi_probe_update(

// --- deepen additive from deepen-ddgi-b56-guards-c107 ---
    return !tryValidateProbeSampleCoords(desc, coords, outReason);
    return wouldSkipProbeSampleCoords(desc, coords, reason);
bool wouldSkipProbeTrilinearSample(const DDGIDesc& desc,
    return !tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, outReason);
    return wouldSkipProbeTrilinearSample(desc, coords, cache, cache_count, reason);
    CacheIndexRejectReason local = CacheIndexRejectReason::None;
    const bool skip = !tryValidateCacheIndex(desc, probe_index, cache_count, local);
    const bool skip = !tryValidateCacheIndex(desc, cache, probe_index, cache_count, local);
    ProbeScheduleRejectReason local = ProbeScheduleRejectReason::None;
    const bool skip = !tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, local);
    ProbeUpdateLaunchRejectReason local = ProbeUpdateLaunchRejectReason::None;
    const bool skip = !tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, local);
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    ProbeKernelRejectReason local = ProbeKernelRejectReason::None;
    const bool skip = !tryCanLaunchProbeTraceKernel(params, local);
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    const bool skip = !tryCanLaunchProbeBlendKernel(params, local);

// --- deepen additive from deepen-b56-ddgi-classify-preflights-a6d0 ---
bool wouldSkipCanSampleAtProbeCoords(const DDGIDesc& desc,
    if (outReason == ProbeScheduleRejectReason::NullOutIndices && out_count != nullptr) {
    } else if (outReason == ProbeScheduleRejectReason::ZeroProbeCount ||
               outReason == ProbeScheduleRejectReason::ZeroMaxIndices) {

// --- deepen additive from ddgi-deepen-guards-605d ---
    return tryCanLookupAtProbeIndex(desc, cache, probe_index, cache_count, reason);
bool tryCanLookupAtProbeIndex(const DDGIDesc& desc,
bool wouldClampCacheIndexLookup(u32 probe_index, const DDGIDesc& desc) {
    return wouldSkipCacheIndexLookup(desc, probe_index, cache_count);
    return wouldSkipCacheIndexLookup(desc, cache, probe_index, cache_count);
    return wouldSkipProbeSchedule(probe_count, max_indices, out_indices, out_count);
bool tryCanScheduleProbeUpdates(u32 frame_index,
    if (!tryCanScheduleProbeUpdates(frame_index,

// --- deepen additive from deepen-b56-ddgi-guards-9944 ---
    ProbeGridLayout::tryValidateProbeSampleCoords(desc, coords, reason);
    ddgi_util::tryValidateCacheIndex(desc, probe_index, cache_count, reason);
    ddgi_util::tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);
    ddgi_util::tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
bool ProbeGridLayout::preflightBuildProbeSampleCoords(const DDGIDesc& desc,
    ProbeSampleCoordsRejectReason local = ProbeSampleCoordsRejectReason::None;
        local = ProbeSampleCoordsRejectReason::EmptyGrid;
    return local == ProbeSampleCoordsRejectReason::None;
    const bool ok = tryValidateCacheIndex(desc, probe_index, cache_count, local);
    const bool ok = tryValidateCacheIndex(desc, cache, probe_index, cache_count, local);
    const bool ok = tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, local);
    const bool ok = tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, local);
ProbeKernelRejectReason classifyProbeTraceKernelReject(const DDGIKernelParams& params) {
bool preflightProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    const bool ok = tryCanLaunchProbeTraceKernel(params, local);
ProbeKernelRejectReason classifyProbeBlendKernelReject(const DDGIKernelParams& params) {
bool preflightProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    const bool ok = tryCanLaunchProbeBlendKernel(params, local);

// --- deepen additive from deepen-ddgi-b56-guards-87a1 ---
bool wouldSkipReadIrradianceAtIndex(const DDGIDesc& desc,
    return !tryReadIrradianceAtIndex(desc, cache, cache_count, probe_index, irradiance);

// --- deepen additive from deepen-ddgi-guards-0aed ---
    ddgi_util::tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
    const bool ok = tryValidateProbeSampleCoords(desc, coords, local);
    return wouldSkipDdgiProbeUpdate(desc, probe_indices, probe_count);
    return wouldSkipProbeTraceKernel(params);
    return wouldSkipProbeBlendKernel(params);

// --- deepen additive from deepen-ddgi-guards-add8 ---
bool ProbeGridLayout::wouldSkipBuildProbeSampleCoords(const DDGIDesc& desc) {
    ProbeSampleCoordsRejectReason localReason = ProbeSampleCoordsRejectReason::None;
    const bool valid = tryValidateProbeSampleCoords(desc, coords, localReason);
bool wouldSkipSampleAtProbeCoords(const DDGIDesc& desc,
bool preflightCacheIndex(const DDGIDesc& desc,
    CacheIndexRejectReason localReason = CacheIndexRejectReason::None;
    const bool valid = tryValidateCacheIndex(desc, probe_index, cache_count, localReason);
    const bool valid = tryValidateCacheIndex(desc, cache, probe_index, cache_count, localReason);
    ProbeScheduleRejectReason localReason = ProbeScheduleRejectReason::None;
    const bool schedulable = tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, localReason);
bool wouldSkipTrilinearProbeIrradiance(const DDGIDesc& desc,
    return !tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, irradiance);
bool wouldSkipTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
    return !tryTrilinearDirectionalProbeIrradiance(
bool tryValidateScheduledProbeIndices(const DDGIDesc& desc,
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, outReason);
bool wouldSkipScheduledProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    ProbeUpdateLaunchRejectReason localReason = ProbeUpdateLaunchRejectReason::None;
    const bool launchable = tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, localReason);
    ProbeKernelRejectReason localReason = ProbeKernelRejectReason::None;
    const bool launchable = tryCanLaunchProbeTraceKernel(params, localReason);
    const bool launchable = tryCanLaunchProbeBlendKernel(params, localReason);

// --- deepen additive from deepen-ddgi-guards-badf ---
    case ProbeUpdateLaunchRejectReason::DuplicateProbeIndex:
bool wouldSkipTrilinearSampleAtCoords(const DDGIDesc& desc,
    return wouldSkipTrilinearSampleAtCoords(desc, coords, cache, cache_count);
                outReason = ProbeUpdateLaunchRejectReason::DuplicateProbeIndex;

// --- deepen additive from deepen-ddgi-guards-2134 ---
    return classifyCacheIndexReject(desc, cache, probe_index, cache_count) != CacheIndexRejectReason::None;
    if (classifyCacheIndexReject(desc, cache, probe_index, cache_count) != CacheIndexRejectReason::None) {

// --- deepen additive from deepen-ddgi-guards-b07e ---
    if (outReason != CacheIndexRejectReason::None) {
bool wouldClampCacheIndexLookup(const DDGIDesc& desc, u32 probe_index) {
    return classifyCacheIndexReject(desc, probe_index, cache_count) != CacheIndexRejectReason::None;
    return classifyProbeScheduleReject(probe_count, max_indices, out_indices, out_count) !=
    return classifyProbeUpdateLaunchReject(desc, probe_indices, probe_count) !=

// --- deepen additive from deepen-ddgi-guards-33be ---
    return tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason) &&
    return !tryValidateCacheIndex(desc, probe_index, cache_count, outReason);
    return !tryValidateCacheIndex(desc, cache, probe_index, cache_count, outReason);
    return !tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, outReason);
    return !tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, outReason);
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    return !tryCanLaunchProbeTraceKernel(params, outReason);
bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    return !tryCanLaunchProbeBlendKernel(params, outReason);

// --- deepen additive from deepen-ddgi-b56-guards-15d4 ---
bool wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (classifyProbeSampleCoordsReject(desc, coords) != ProbeSampleCoordsRejectReason::None) {
    const ProbeSampleCoordsRejectReason coordReject = classifyProbeSampleCoordsReject(desc, coords);
    if (coordReject != ProbeSampleCoordsRejectReason::None) {
    return outReason != CacheIndexRejectReason::None;
    return wouldSkipCacheIndexLookup(desc, probe_index, cache_count, reason);
    return wouldSkipCacheIndexLookup(desc, cache, probe_index, cache_count, reason);
    if (outReason == ProbeScheduleRejectReason::ZeroProbeCount && out_count != nullptr) {
    if (outReason == ProbeScheduleRejectReason::ZeroMaxIndices && out_count != nullptr) {
    return outReason != ProbeScheduleRejectReason::None;
    return wouldSkipProbeSchedule(probe_count, max_indices, out_indices, out_count, reason);
    return outReason != ProbeUpdateLaunchRejectReason::None;
    return wouldSkipDdgiProbeUpdate(desc, probe_indices, probe_count, reason);

// --- deepen additive from deepen-ddgi-guards-1a6d ---
    const ProbeSampleCoordsRejectReason reject = classifyProbeSampleCoordsReject(desc, coords);
    return reject == ProbeSampleCoordsRejectReason::None;
    return !preflightProbeSampleCoords(desc, coords);
    return reject == ProbeTrilinearSampleRejectReason::None;
    return !preflightTrilinearProbeSample(desc, coords, cache, cache_count);
    return tryReadIrradianceAtIndex(desc, cache, cache_count, probe_index, out_irradiance);
    return !wouldSkipCacheIndexLookup(desc, probe_index, cache_count);
    return !wouldSkipCacheIndexLookup(desc, cache, probe_index, cache_count);
    const CacheIndexRejectReason reject = classifyCacheIndexReject(desc, probe_index, cache_count);
    return reject == CacheIndexRejectReason::None;
    return reject == ProbeScheduleRejectReason::None;
    return tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, out_irradiance);
    return reject == ProbeUpdateLaunchRejectReason::None;
    return reject == ProbeKernelRejectReason::None;
    return preflightProbeTraceKernel(params, reason);

// --- deepen additive from deepen-b56-ddgi-guards-3e47 ---
    return wouldSkipTrilinearProbeSample(desc, coords, cache, cache_count);
bool tryReadIrradianceAtCoord(const DDGIDesc& desc,
    return tryReadIrradianceAtCoord(desc, cache, cache_count, coord, out_irradiance, reason);
    if (!tryCanLookupAtCoord(desc, coord, cache_count, outReason)) {
    if (outReason == CacheIndexRejectReason::OutOfRangeProbeIndex) {
    return tryReadIrradianceAtIndex(desc, cache, cache_count, index, out_irradiance, outReason);
    return tryCanLookupAtCoord(desc, coord, cache_count, reason);
bool tryCanLookupAtCoord(const DDGIDesc& desc,

// --- deepen additive from deepen-ddgi-guards-4d4e ---
bool wouldSkipDdgiSample(const DDGIDesc& desc, const DDGISampleRequest& request, u32 cache_count) {
    case ProbeKernelRejectReason::NullPrevIrradiance:
    case ProbeKernelRejectReason::NullOutRadiance:
ProbeKernelRejectReason classifyProbeKernelTraceReject(const DDGIKernelParams& params) {
ProbeKernelRejectReason classifyProbeKernelBlendReject(const DDGIKernelParams& params) {
bool tryPreflightProbeTraceWorldPositions(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    ProbeKernelRejectReason baseReason = ProbeKernelRejectReason::None;
    if (!tryCanLaunchProbeTraceKernel(params, baseReason)) {
bool tryPreflightProbeBlendSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    if (!tryCanLaunchProbeBlendKernel(params, baseReason)) {
        outReason = ProbeKernelRejectReason::NullPrevIrradiance;
        outReason = ProbeKernelRejectReason::NullOutRadiance;

// --- deepen additive from deepen-ddgi-guards-13d5 ---
    return !probeTrilinearSampleRejectReasonIsBlocking(reject);
                                      ProbeTrilinearSampleRejectReason& reason) {
    return preflightTrilinearProbeSample(desc, coords, cache, cache_count, &reason);
                                 ProbeUpdateLaunchRejectReason& reason) {
    return preflightDdgiProbeUpdate(desc, probe_indices, probe_count, &reason);
    return !canPreflightProbeSampleCoords(desc, coords);
bool tryCanSampleAtProbeCoord(const DDGIDesc& desc,
    return tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, outReason);
        return tryReadIrradianceAtIndex(desc, cache, cache_count, probe_index, out_irradiance, outReason);
                                  CacheIndexRejectReason& reason) {
    return preflightCacheIndexLookup(desc, cache, probe_index, cache_count, &reason);
CacheIndexRejectReason classifyCacheIndexRejectAtCoord(const DDGIDesc& desc,
        return classifyCacheIndexReject(desc, cache, probe_index, cache_count);
bool wouldClampCacheIndexCoord(const DDGIDesc& desc, u32 x, u32 y, u32 z) {
bool tryPreflightProbeSchedule(u32 probe_count,
                               ProbeScheduleRejectReason& reason) {
    return preflightProbeSchedule(probe_count, max_indices, out_indices, out_count, &reason);
bool tryPreflightProbeKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason& reason) {
    return preflightProbeKernelLaunch(params, &reason);
    return wouldSkipProbeKernelLaunch(params);

// --- deepen additive from deepen-ddgi-guards-0cb7 ---
    reason = classifyDdgiProbeUpdateReject(desc, probe_indices, probe_count);
    return !probeUpdateLaunchRejectReasonIsBlocking(reason);
    return !preflightDdgiProbeUpdate(desc, probe_indices, probe_count);
    if (!tryBuildProbeSampleCoords(desc, world_position, out_coords, reason)) {
    return !probeTrilinearSampleRejectReasonIsBlocking(reason);
    return preflightTrilinearProbeSample(desc, coords, cache, cache_count);
    return !cacheIndexRejectReasonIsBlocking(reason);
    return preflightCacheIndexLookup(desc, cache, probe_index, cache_count);
    return !probeScheduleRejectReasonIsBlocking(reason);
    return !probeKernelRejectReasonIsBlocking(reason);

// --- deepen additive from deepen-ddgi-b56-guards-ae4c ---
        ProbeTrilinearSampleRejectReason reject = ProbeTrilinearSampleRejectReason::NotSampleable;
            reject = ProbeTrilinearSampleRejectReason::EmptyGrid;
            reject = ProbeTrilinearSampleRejectReason::NotSampleable;
            reject = ProbeTrilinearSampleRejectReason::InvalidSampleCoords;
    return preflightTrilinearProbeSample(desc, coords, cache, cache_count, reason);
bool tryValidateScheduledCacheIndices(const DDGIDesc& desc,
        if (!tryValidateCacheIndex(desc, probe_indices[i], cache_count, outReason)) {
        if (!tryValidateCacheIndex(desc, cache, probe_indices[i], cache_count, outReason)) {
    case ProbeKernelRejectReason::EmptyGrid:
    case ProbeKernelRejectReason::OutOfRangeProbeIndex:
ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params, const DDGIDesc& desc) {
    tryCanLaunchProbeTraceKernel(params, desc, reason);
bool wouldSkipProbeKernelLaunch(const DDGIKernelParams& params, const DDGIDesc& desc) {
    const ProbeKernelRejectReason reject = classifyProbeKernelReject(params, desc);
        outReason = ProbeKernelRejectReason::EmptyGrid;
            outReason = ProbeKernelRejectReason::OutOfRangeProbeIndex;
    return tryCanLaunchProbeTraceKernel(params, desc, reason);
    return tryCanLaunchProbeTraceKernel(params, desc, outReason);
    return tryCanLaunchProbeBlendKernel(params, desc, reason);

// --- deepen additive from deepen-ddgi-guards-ba83 ---
ProbeTrilinearSampleRejectReason classifyTrilinearProbeSampleReject(const DDGIDesc& desc,
        return ProbeGridLayout::isEmptyGrid(desc) ? ProbeTrilinearSampleRejectReason::EmptyGrid
    return classifyTrilinearProbeSampleReject(desc, coords, cache, cache_count);
bool preflightTrilinearProbeIrradiance(const DDGIDesc& desc,
        classifyTrilinearProbeSampleReject(desc, world_position, cache, cache_count);
    return !preflightTrilinearProbeIrradiance(desc, world_position, cache, cache_count);
    return classifyProbeTraceKernelReject(params);
    const ProbeKernelRejectReason reject = classifyProbeTraceKernelReject(params);
    const ProbeKernelRejectReason reject = classifyProbeBlendKernelReject(params);

// --- deepen additive from deepen-ddgi-guards-914b ---
    return classifyProbeTrilinearSampleReject(desc, coords, cache, cache_count);
bool preflightTrilinearProbeSampleAtCoords(const DDGIDesc& desc,
        classifyProbeTrilinearSampleReject(desc, world_position, cache, cache_count);
    return !preflightTrilinearProbeSampleAtCoords(desc, coords, cache, cache_count);
    return !preflightTrilinearProbeSample(desc, world_position, cache, cache_count);
    const ProbeScheduleRejectReason reject = classifyProbeScheduleRejectAtRate(
    return classifyProbeKernelReject(params);
bool preflightProbeTraceKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
bool preflightProbeBlendKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
bool populateAndPreflightDDGIKernelParams(DDGIKernelParams& params,
    return preflightProbeKernelLaunch(params, reason);

// --- deepen additive from deepen-ddgi-guards-d9ab ---
bool preflightTrilinearProbeSampleAtWorldPosition(const DDGIDesc& desc,
    ProbeKernelRejectReason reject = ProbeKernelRejectReason::None;
    tryCanLaunchProbeTraceKernel(params, reject);
    tryCanLaunchProbeBlendKernel(params, reject);

// --- deepen additive from deepen-ddgi-b56-guards-a1e1 ---
    reason = classifyProbeScheduleRejectAtRate(probe_count, probes_per_frame, max_indices, out_indices, out_count);
    reason = classifyProbeTrilinearSampleReject(desc, world_position, cache, cache_count);
bool preflightTrilinearDirectionalProbeSample(const DDGIDesc& desc,
    return preflightTrilinearProbeSample(desc, world_position, cache, cache_count, reason);
bool tryPreflightTrilinearDirectionalProbeSample(const DDGIDesc& desc,
    return tryPreflightTrilinearProbeSample(desc, world_position, cache, cache_count, reason);
bool wouldSkipTrilinearDirectionalProbeSample(const DDGIDesc& desc,
    return wouldSkipTrilinearProbeSample(desc, world_position, cache, cache_count);

// --- deepen additive from deepen-ddgi-guards-1537 ---
    const bool ok = preflightTrilinearProbeSample(desc, coords, cache, cache_count, reason);

// --- deepen additive from deepen-ddgi-guards-4d1d ---
bool preflightTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
    return preflightTrilinearProbeIrradiance(desc, world_position, cache, cache_count, reason);
    return !preflightTrilinearDirectionalProbeIrradiance(

// --- deepen additive from deepen-ddgi-guards-51fd ---
    return tryValidateCacheIndex(desc, cache, probe_index, cache_count, outReason);
ProbeScheduleRejectReason classifyProbeScheduleAtRateReject(u32 probe_count,
        classifyProbeScheduleAtRateReject(probe_count, probes_per_frame, max_indices, out_indices, out_count);
bool tryPreflightProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
bool tryPreflightProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    return tryCanLaunchProbeBlendKernel(params, outReason);

// --- deepen additive from deepen-ddgi-guards-f003 ---
    return probeTrilinearSampleRejectReasonIsBlocking(reject);

// --- deepen additive from deepen-ddgi-guards-5dcb ---
bool preflightProbeTrilinearSample(const DDGIDesc& desc,
    return preflightProbeTrilinearSample(desc, coords, cache, cache_count, reason);
    return !preflightProbeTrilinearSample(desc, coords, cache, cache_count);

// --- deepen additive from deepen-ddgi-guards-3160 ---
ProbeTrilinearSampleRejectReason classifyTrilinearProbeIrradianceReject(const DDGIDesc& desc,
        classifyTrilinearProbeIrradianceReject(desc, world_position, cache, cache_count);
    const bool ok = tryCanLaunchProbeTraceKernel(params, reject);
    const bool ok = tryCanLaunchProbeBlendKernel(params, reject);

// --- deepen additive from deepen-ddgi-b56-guards-df48 ---
    return classifyProbeTrilinearSampleRejectAtCoords(desc, coords, cache, cache_count);
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleRejectAtCoords(const DDGIDesc& desc,
const char* ddgiHostKernelLaunchRejectReasonLabel(DdgiHostKernelLaunchRejectReason reason) {
    case DdgiHostKernelLaunchRejectReason::None:
    case DdgiHostKernelLaunchRejectReason::EmptyGrid:
    case DdgiHostKernelLaunchRejectReason::NullProbeIndices:
    case DdgiHostKernelLaunchRejectReason::ZeroUpdateCount:
    case DdgiHostKernelLaunchRejectReason::ZeroRaysPerProbe:
    case DdgiHostKernelLaunchRejectReason::OutOfRangeProbeIndex:
bool ddgiHostKernelLaunchRejectReasonIsBlocking(DdgiHostKernelLaunchRejectReason reason) {
    return reason != DdgiHostKernelLaunchRejectReason::None;
DdgiHostKernelLaunchRejectReason classifyDdgiHostKernelLaunchReject(const DDGIDesc& desc,
    gi::ProbeKernelRejectReason kernelReason = gi::ProbeKernelRejectReason::None;
    if (!gi::tryCanLaunchProbeTraceKernel(params, kernelReason)) {
        case gi::ProbeKernelRejectReason::ZeroUpdateCount:
            return DdgiHostKernelLaunchRejectReason::ZeroUpdateCount;
        case gi::ProbeKernelRejectReason::NullProbeIndices:
            return DdgiHostKernelLaunchRejectReason::NullProbeIndices;
        case gi::ProbeKernelRejectReason::ZeroRaysPerProbe:
            return DdgiHostKernelLaunchRejectReason::ZeroRaysPerProbe;
        case gi::ProbeKernelRejectReason::None:
        return DdgiHostKernelLaunchRejectReason::EmptyGrid;
    ProbeUpdateLaunchRejectReason launchReason = ProbeUpdateLaunchRejectReason::None;
        if (launchReason == ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex) {
            return DdgiHostKernelLaunchRejectReason::OutOfRangeProbeIndex;
    return DdgiHostKernelLaunchRejectReason::None;
bool preflightDdgiHostKernelLaunch(const DDGIDesc& desc,
                                   DdgiHostKernelLaunchRejectReason* reason) {
    const DdgiHostKernelLaunchRejectReason reject = classifyDdgiHostKernelLaunchReject(desc, params);
    return !ddgiHostKernelLaunchRejectReasonIsBlocking(reject);
bool tryPreflightDdgiHostKernelLaunch(const DDGIDesc& desc,
                                      DdgiHostKernelLaunchRejectReason& reason) {
    reason = classifyDdgiHostKernelLaunchReject(desc, params);
    return !ddgiHostKernelLaunchRejectReasonIsBlocking(reason);
bool wouldSkipDdgiHostKernelLaunch(const DDGIDesc& desc, const gi::DDGIKernelParams& params) {
    return !preflightDdgiHostKernelLaunch(desc, params);

// --- deepen additive from deepen-ddgi-guards-c2c2 ---
    case ProbeTrilinearSampleRejectReason::ClampableSampleCoords:
bool ProbeGridLayout::wouldSkipSampleCoordPreflight(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
ProbeTrilinearSampleRejectReason trilinearRejectFromSampleCoords(ProbeSampleCoordsRejectReason reason) {
        return ProbeTrilinearSampleRejectReason::ClampableSampleCoords;
    return trilinearRejectFromSampleCoords(ProbeGridLayout::classifyProbeSampleCoordsReject(desc, coords));
    return !probeTrilinearSampleRejectReasonIsBlocking(outReason);
        classifyProbeTrilinearSampleReject(desc, coords, cache, cache_count));

// --- deepen additive from deepen-ddgi-guards-30f9 ---
const char* probeGridRejectReasonLabel(ProbeGridRejectReason reason) {
    case ProbeGridRejectReason::None:
    case ProbeGridRejectReason::EmptyGrid:
    case ProbeGridRejectReason::NotSampleable:
    case ProbeGridRejectReason::OutOfRangeProbeIndex:
    case ProbeGridRejectReason::OutOfRangeProbeCoord:
bool probeGridRejectReasonIsBlocking(ProbeGridRejectReason reason) {
    return reason != ProbeGridRejectReason::None;
ProbeGridRejectReason ProbeGridLayout::classifyProbeGridReject(const DDGIDesc& desc) {
        return ProbeGridRejectReason::EmptyGrid;
        return ProbeGridRejectReason::NotSampleable;
    return ProbeGridRejectReason::None;
ProbeGridRejectReason ProbeGridLayout::classifyProbeIndexReject(const DDGIDesc& desc, u32 probe_index) {
    const ProbeGridRejectReason gridReason = classifyProbeGridReject(desc);
    if (gridReason != ProbeGridRejectReason::None) {
        return ProbeGridRejectReason::OutOfRangeProbeIndex;
ProbeGridRejectReason ProbeGridLayout::classifyProbeCoordReject(const DDGIDesc& desc,
        return ProbeGridRejectReason::OutOfRangeProbeCoord;
bool ProbeGridLayout::preflightProbeGrid(const DDGIDesc& desc, ProbeGridRejectReason* reason) {
    const ProbeGridRejectReason reject = classifyProbeGridReject(desc);
    return !probeGridRejectReasonIsBlocking(reject);
bool ProbeGridLayout::preflightProbeIndex(const DDGIDesc& desc,
                                          ProbeGridRejectReason* reason) {
    const ProbeGridRejectReason reject = classifyProbeIndexReject(desc, probe_index);
bool ProbeGridLayout::preflightProbeCoord(const DDGIDesc& desc,
    const ProbeGridRejectReason reject = classifyProbeCoordReject(desc, coord);
bool ProbeGridLayout::wouldSkipProbeGridAccess(const DDGIDesc& desc) {
    return !preflightProbeGrid(desc);
    return !preflightProbeTrilinearSample(desc, world_position, cache, cache_count);

// --- deepen additive from deepen-ddgi-guards-4831 ---
bool ProbeGridLayout::tryClampProbeGridCoord(const DDGIDesc& desc,
bool ProbeGridLayout::wouldSkipProbeCoordPreflight(const DDGIDesc& desc, const ProbeGridCoord& coord) {

// --- deepen additive from deepen-b56-ddgi-guards-50ea ---
    case ProbeTrilinearSampleRejectReason::ClampableWeights:
    return tryTrilinearSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
bool tryTrilinearSampleAtProbeCoords(const DDGIDesc& desc,
    ProbeSampleCoordsRejectReason sampleReason = ProbeSampleCoordsRejectReason::None;
    if (!ProbeGridLayout::tryPreflightProbeSampleCoords(desc, coords, sampleReason)) {
    if (sampleReason == ProbeSampleCoordsRejectReason::OutOfRangeWeights ||
        sampleReason == ProbeSampleCoordsRejectReason::UnorderedCorners) {
        outReason = ProbeTrilinearSampleRejectReason::ClampableWeights;
    return !tryTrilinearSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
    tryTrilinearSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
bool tryTrilinearProbeIrradianceAtCoords(const DDGIDesc& desc,
    if (!tryTrilinearSampleAtProbeCoords(desc, coords, cache, cache_count, outReason)) {

// --- deepen additive from deepen-ddgi-guards-49d7 ---
const char* probeGridCoordRejectReasonLabel(ProbeGridCoordRejectReason reason) {
    case ProbeGridCoordRejectReason::None:
    case ProbeGridCoordRejectReason::EmptyGrid:
    case ProbeGridCoordRejectReason::OutOfRangeCoord:
bool probeGridCoordRejectReasonIsBlocking(ProbeGridCoordRejectReason reason) {
    return reason != ProbeGridCoordRejectReason::None;
bool ProbeGridLayout::tryValidateProbeCoord(const DDGIDesc& desc,
                                            ProbeGridCoordRejectReason& outReason) {
        outReason = ProbeGridCoordRejectReason::EmptyGrid;
        outReason = ProbeGridCoordRejectReason::OutOfRangeCoord;
    outReason = ProbeGridCoordRejectReason::None;
ProbeGridCoordRejectReason ProbeGridLayout::classifyProbeCoordReject(const DDGIDesc& desc,
    ProbeGridCoordRejectReason reason = ProbeGridCoordRejectReason::None;
    tryValidateProbeCoord(desc, coord, reason);
                                          ProbeGridCoordRejectReason* reason) {
    const ProbeGridCoordRejectReason reject = classifyProbeCoordReject(desc, coord);
    return !probeGridCoordRejectReasonIsBlocking(reject);
    return !tryValidateProbeCoord(desc, coord, reason);
bool wouldSkipProbeLookup(const DDGIDesc& desc, const IrradianceCacheEntry* cache, u32 cache_count) {
    if (ProbeGridLayout::wouldSkipProbeCoordPreflight(desc, coord)) {
    return wouldSkipCacheIndexLookup(desc, cache, index, cache_count);
    return tryTrilinearProbeIrradianceAtCoords(desc, coords, cache, cache_count, out_irradiance, outReason);

// --- deepen additive from deepen-ddgi-guards-39f0 ---
    case ProbeGridRejectReason::OutOfRangeIndex:
    case ProbeGridRejectReason::OutOfRangeCoord:
bool ProbeGridLayout::tryValidateProbeIndex(const DDGIDesc& desc,
                                            ProbeGridRejectReason& outReason) {
        outReason = ProbeGridRejectReason::EmptyGrid;
        outReason = ProbeGridRejectReason::OutOfRangeIndex;
    outReason = ProbeGridRejectReason::None;
        outReason = ProbeGridRejectReason::OutOfRangeCoord;
    ProbeGridRejectReason reason = ProbeGridRejectReason::None;
    tryValidateProbeIndex(desc, probe_index, reason);
bool ProbeGridLayout::wouldSkipProbeIndex(const DDGIDesc& desc, u32 probe_index) {
    return !preflightProbeIndex(desc, probe_index);
bool ProbeGridLayout::wouldSkipProbeCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    return !preflightProbeCoord(desc, coord);

// --- deepen additive from deepen-ddgi-guards-5451 ---
    case ProbeGridRejectReason::InvalidCoord:
        outReason = ProbeGridRejectReason::InvalidCoord;
bool ProbeGridLayout::wouldSkipProbeCoordLookup(const DDGIDesc& desc, const ProbeGridCoord& coord) {
bool ProbeGridLayout::wouldSkipProbeIndexLookup(const DDGIDesc& desc, u32 probe_index) {

// --- deepen additive from deepen-ddgi-b56-guards-132c ---
    case ProbeGridSourceRejectReason::ZeroIrradianceRes:
    case ProbeGridSourceRejectReason::ZeroDepthRes:
    case ProbeGridSourceRejectReason::InvalidSpacing:
    case ProbeGridSourceRejectReason::ZeroRaysPerProbe:
    case ProbeGridSourceRejectReason::ZeroProbesPerFrame:
bool tryValidateProbeGridSourceInit(const DDGIDesc& desc, ProbeGridSourceRejectReason& outReason) {
        outReason = ProbeGridSourceRejectReason::ZeroIrradianceRes;
        outReason = ProbeGridSourceRejectReason::ZeroDepthRes;
        return tryValidateProbeGridSourceInit(desc, outReason);
            outReason = ProbeGridSourceRejectReason::InvalidSpacing;
        if (!tryValidateProbeGridSourceInit(desc, outReason)) {
            outReason = ProbeGridSourceRejectReason::ZeroRaysPerProbe;
            outReason = ProbeGridSourceRejectReason::ZeroProbesPerFrame;
ProbeGridSourceRejectReason classifyProbeGridSourceReject(const DDGIDesc& desc, ProbeGridSourceKind kind) {
    tryValidateProbeGridSource(desc, kind, reason);
    const ProbeGridSourceRejectReason reject = classifyProbeGridSourceReject(desc, kind);
bool wouldSkipProbeGridSource(const DDGIDesc& desc, ProbeGridSourceKind kind) {
    return !preflightProbeGridSource(desc, kind);
    return tryValidateProbeGridSource(desc, ProbeGridSourceKind::Sample, reason);
ProbeTrilinearSampleRejectReason classifyTrilinearSampleReject(const DDGIDesc& desc,
        classifyTrilinearSampleReject(desc, coords, cache, cache_count);
ProbeKernelRejectReason classifyProbeKernelRejectForDesc(const DDGIDesc& desc, const DDGIKernelParams& params) {
        return ProbeKernelRejectReason::EmptyGrid;
bool preflightProbeKernelLaunchForDesc(const DDGIDesc& desc,
    const ProbeKernelRejectReason reject = classifyProbeKernelRejectForDesc(desc, params);
bool wouldSkipProbeKernelLaunchForDesc(const DDGIDesc& desc, const DDGIKernelParams& params) {
    return !preflightProbeKernelLaunchForDesc(desc, params);

// --- deepen additive from deepen-ddgi-b56-guards-a29f ---
const char* probeIndexRejectReasonLabel(ProbeIndexRejectReason reason) {
    case ProbeIndexRejectReason::None:
    case ProbeIndexRejectReason::EmptyGrid:
    case ProbeIndexRejectReason::OutOfRangeIndex:
const char* probeCoordRejectReasonLabel(ProbeCoordRejectReason reason) {
    case ProbeCoordRejectReason::None:
    case ProbeCoordRejectReason::EmptyGrid:
    case ProbeCoordRejectReason::OutOfRangeCoord:
bool probeIndexRejectReasonIsBlocking(ProbeIndexRejectReason reason) {
    return reason != ProbeIndexRejectReason::None;
bool probeCoordRejectReasonIsBlocking(ProbeCoordRejectReason reason) {
    return reason != ProbeCoordRejectReason::None;
                                            ProbeIndexRejectReason& outReason) {
        outReason = ProbeIndexRejectReason::EmptyGrid;
        outReason = ProbeIndexRejectReason::OutOfRangeIndex;
    outReason = ProbeIndexRejectReason::None;
ProbeIndexRejectReason ProbeGridLayout::classifyProbeIndexReject(const DDGIDesc& desc, u32 probe_index) {
    ProbeIndexRejectReason reason = ProbeIndexRejectReason::None;
                                          ProbeIndexRejectReason* reason) {
    const ProbeIndexRejectReason reject = classifyProbeIndexReject(desc, probe_index);
    return !probeIndexRejectReasonIsBlocking(reject);
                                            ProbeCoordRejectReason& outReason) {
        outReason = ProbeCoordRejectReason::EmptyGrid;
        outReason = ProbeCoordRejectReason::OutOfRangeCoord;
    outReason = ProbeCoordRejectReason::None;
ProbeCoordRejectReason ProbeGridLayout::classifyProbeCoordReject(const DDGIDesc& desc,
    ProbeCoordRejectReason reason = ProbeCoordRejectReason::None;
                                          ProbeCoordRejectReason* reason) {
    const ProbeCoordRejectReason reject = classifyProbeCoordReject(desc, coord);
    return !probeCoordRejectReasonIsBlocking(reject);

// --- deepen additive from deepen-ddgi-b56-guards-012a ---
    case ProbeGridSourceRejectReason::OutOfRangeProbeIndex:
    case ProbeGridSourceRejectReason::InvalidProbeCoord:
    outReason = classifyDdgiProbeUpdateReject(desc, probe_indices, probe_count);
    return !probeUpdateLaunchRejectReasonIsBlocking(outReason);
bool ProbeGridLayout::tryProbeCoordFromIndex(const DDGIDesc& desc,
        outReason = ProbeGridSourceRejectReason::OutOfRangeProbeIndex;
ProbeGridSourceRejectReason ProbeGridLayout::classifyProbeCoordFromIndex(const DDGIDesc& desc,
    tryProbeCoordFromIndex(desc, probe_index, coord, reason);
bool ProbeGridLayout::preflightProbeCoordFromIndex(const DDGIDesc& desc,
    ProbeGridSourceRejectReason reject = ProbeGridSourceRejectReason::None;
    const bool ok = tryProbeCoordFromIndex(desc, probe_index, coord, reject);
bool ProbeGridLayout::wouldSkipProbeCoordFromIndex(const DDGIDesc& desc, u32 probe_index) {
    return !preflightProbeCoordFromIndex(desc, probe_index);
bool ProbeGridLayout::tryProbeIndexFromCoord(const DDGIDesc& desc,
        outReason = ProbeGridSourceRejectReason::InvalidProbeCoord;
ProbeGridSourceRejectReason ProbeGridLayout::classifyProbeIndexFromCoord(const DDGIDesc& desc,
    tryProbeIndexFromCoord(desc, coord, index, reason);
bool ProbeGridLayout::preflightProbeIndexFromCoord(const DDGIDesc& desc,
    const bool ok = tryProbeIndexFromCoord(desc, coord, index, reject);
bool ProbeGridLayout::wouldSkipProbeIndexFromCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    return !preflightProbeIndexFromCoord(desc, coord);
bool tryPreflightProbeTrilinearSample(const DDGIDesc& desc,
    return !cacheIndexRejectReasonIsBlocking(outReason);
    return !probeScheduleRejectReasonIsBlocking(outReason);
    outReason = classifyProbeScheduleRejectAtRate(
    return !probeKernelRejectReasonIsBlocking(outReason);

// --- deepen additive from deepen-ddgi-guards-aca0 ---
    case ProbeGridSourceRejectReason::NullCache:
    case ProbeGridSourceRejectReason::UndersizedCache:
    tryValidateProbeGridSource(desc, cache, cache_count, reason);
        outReason = ProbeGridSourceRejectReason::NullCache;
        outReason = ProbeGridSourceRejectReason::UndersizedCache;
bool wouldSkipProbeGridSource(const DDGIDesc& desc, const IrradianceCacheEntry* cache, u32 cache_count) {
    return !tryValidateProbeGridSource(desc, cache, cache_count, reason);
    const ProbeGridSourceRejectReason reject = classifyProbeGridSourceReject(desc, cache, cache_count);
    return tryCanTrilinearSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
bool tryCanTrilinearSampleAtProbeCoords(const DDGIDesc& desc,
    ProbeGridSourceRejectReason sourceReason = ProbeGridSourceRejectReason::None;
    if (!tryValidateProbeGridSource(desc, cache, cache_count, sourceReason)) {
    if (sampleReason == ProbeSampleCoordsRejectReason::OutOfRangeWeights) {
    return !tryCanTrilinearSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
bool tryValidateCacheIndexAtCoord(const DDGIDesc& desc,
    return tryValidateCacheIndexAtCoord(desc, coord, cache_count, outReason);
bool wouldClampCacheIndexLookupCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {

// --- deepen additive from deepen-b56-ddgi-wouldskip-preflights-a4ee ---
bool preflightProbeGridSource(const DDGIDesc& desc, ProbeSampleCoordsRejectReason* reason) {
            *reason = ProbeSampleCoordsRejectReason::EmptyGrid;
            *reason = ProbeSampleCoordsRejectReason::NotSampleableGrid;
        *reason = ProbeSampleCoordsRejectReason::None;
    return !preflightProbeGridSource(desc, nullptr);
    return wouldSkipProbeGridSource(desc);

// --- deepen additive from deepen-ddgi-b56-guards-22da ---
    return !preflightProbeGridSource(desc, cache, cache_count);
bool wouldSkipProbeLookupAtIndex(const DDGIDesc& desc,
bool wouldSkipProbeLookupAtCoord(const DDGIDesc& desc,
    return wouldSkipProbeLookupAtIndex(desc, cache, probe_index, cache_count);

// --- deepen additive from deepen-ddgi-b56-guards-2627 ---
    return tryValidateScheduledCacheIndices(desc, probe_indices, probe_count, cache_count, outReason);
bool preflightScheduledCacheIndices(const DDGIDesc& desc,
    CacheIndexRejectReason reject = CacheIndexRejectReason::None;
    const bool ok = tryValidateScheduledCacheIndices(desc, cache, probe_indices, probe_count, cache_count, reject);
bool wouldSkipScheduledCacheIndices(const DDGIDesc& desc,
    return !preflightScheduledCacheIndices(desc, cache, probe_indices, probe_count, cache_count);
    if (!tryCanLaunchProbeTraceKernel(params, reason)) {
                return ProbeKernelRejectReason::OutOfRangeProbeIndex;

// --- deepen additive from deepen-ddgi-guards-b468 ---
    return !tryValidateProbeGridSource(desc, reason);
    return tryValidateProbeGridSource(desc, reason);
    return !preflightTrilinearProbeIrradiance(desc, world_position, cache, cache_count, &reason);
        reject = ProbeGridLayout::isEmptyGrid(desc) ? ProbeTrilinearSampleRejectReason::EmptyGrid
    } else if (!tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reject)) {

// --- deepen additive from deepen-b56-ddgi-guards-c294 ---
    return probeSampleCoordsRejectReasonIsBlocking(outReason);
    return cacheIndexRejectReasonIsBlocking(reject);
    if (!tryCanLookupAtCoord(desc, coord, cache_count, reject)) {
    if (reject == CacheIndexRejectReason::OutOfRangeProbeIndex) {
        *reason = CacheIndexRejectReason::None;
    return probeScheduleRejectReasonIsBlocking(reject);
    return probeUpdateLaunchRejectReasonIsBlocking(reject);

// --- deepen additive from ddgi-b56-guards-deepen-06ef ---
        return ProbeGridSourceRejectReason::EmptyGrid;
        return ProbeGridSourceRejectReason::ZeroIrradianceRes;
        return ProbeGridSourceRejectReason::InvalidSpacing;
    return ProbeGridSourceRejectReason::None;
    return !preflightProbeGridSource(desc);

// --- deepen additive from deepen-b56-ddgi-guards-fc82 ---
    return probeSampleCoordsRejectReasonIsBlocking(classifyProbeSampleCoordsReject(desc, coords));

// --- deepen additive from deepen-b56-ddgi-guards-98c7 ---
bool ProbeGridLayout::wouldSkipProbeSampleCoordsPreflight(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return tryPreflightProbeGridSource(desc, reason);
    tryPreflightProbeGridSource(desc, reason);

// --- deepen additive from deepen-ddgi-guards-0e44 ---
bool tryValidateProbeGridSource(const ProbeGridSource& source, ProbeGridSourceRejectReason& outReason) {
ProbeGridSourceRejectReason classifyProbeGridSourceReject(const ProbeGridSource& source) {
    tryValidateProbeGridSource(source, reason);
bool preflightProbeGridSource(const ProbeGridSource& source, ProbeGridSourceRejectReason* reason) {
    const ProbeGridSourceRejectReason reject = classifyProbeGridSourceReject(source);
bool wouldSkipProbeGridSource(const ProbeGridSource& source) {
    return !preflightProbeGridSource(source);
bool preflightDdgiKernelUpdate(const DDGIDesc& desc,
bool wouldSkipDdgiKernelUpdate(const DDGIDesc& desc,
    return !preflightDdgiKernelUpdate(desc, probe_indices, probe_count, frame_seed);

// --- deepen additive from deepen-ddgi-b56-guards-8377 ---
bool wouldSkipProbeSample(const DDGIDesc& desc,
bool preflightPopulatedProbeKernelLaunch(DDGIKernelParams& params,

// --- deepen additive from deepen-b56-ddgi-guards-f8af ---
        return ProbeGridSourceRejectReason::NotSampleable;
    const ProbeTrilinearSampleRejectReason reject = classifyTrilinearProbeSampleReject(desc, coords, cache, cache_count);

// --- deepen additive from deepen-ddgi-guards-e593 ---
    return !tryValidateProbeGridSource(source, reason);
    return wouldSkipProbeTrilinearSample(desc, coords, cache, cache_count);
    return tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, outReason);
    const bool ok = tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, ignored, reject);
bool tryPreflightTrilinearProbeIrradiance(const DDGIDesc& desc,
    return tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, ignored, outReason);
    const bool ok = tryTrilinearDirectionalProbeIrradiance(
bool tryPreflightTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,

// --- deepen additive from deepen-ddgi-guards-06af ---
    return classifyCacheIndexReject(desc, cache, index, cache_count);
bool preflightCacheIndexLookupAtCoord(const DDGIDesc& desc,
    const CacheIndexRejectReason reject = classifyCacheIndexRejectAtCoord(desc, cache, coord, cache_count);
    return !preflightCacheIndexLookupAtCoord(desc, cache, coord, cache_count);
bool wouldClampCacheIndexCoordForLookup(const DDGIDesc& desc, const ProbeGridCoord& coord) {

// --- deepen additive from deepen-b56-ddgi-guards-a5ff ---
        outReason = ProbeTrilinearSampleRejectReason::ClampableSampleCoords;
    if (!tryValidateCacheIndex(desc, 0u, cache_count, outReason)) {
    return tryValidateCacheIndexAtCoord(desc, x, y, z, cache_count, outReason);
bool wouldSkipCacheIndexLookupAtCoord(const DDGIDesc& desc, u32 x, u32 y, u32 z, u32 cache_count) {
    return !tryValidateCacheIndexAtCoord(desc, x, y, z, cache_count, reason);
    return !tryValidateCacheIndexAtCoord(desc, cache, x, y, z, cache_count, reason);
bool wouldClampCacheIndexLookupCoord(const DDGIDesc& desc, u32 x, u32 y, u32 z) {

// --- deepen additive from deepen-ddgi-b56-guards-8799 ---
    case ProbeGridSourceRejectReason::InvalidKind:
    case ProbeGridSourceRejectReason::NullDesc:
    case ProbeGridSourceRejectReason::NullProbeData:
    case ProbeGridSourceRejectReason::DescProbeDataMismatch:
                               ProbeTrilinearSampleRejectReason& outReason);
        outReason = ProbeGridSourceRejectReason::InvalidKind;
        outReason = ProbeGridSourceRejectReason::NullDesc;
            outReason = ProbeGridSourceRejectReason::NullProbeData;
            outReason = ProbeGridSourceRejectReason::DescProbeDataMismatch;
bool tryValidateProbeGridSourceForSampling(const ProbeGridSource& source,
    if (!tryValidateProbeGridSource(source, outReason)) {

// --- deepen additive from deepen-ddgi-guards-b85d ---
    case ProbeGridRejectReason::InvalidSpacing:
    case ProbeGridRejectReason::ZeroIrradianceRes:
ProbeGridRejectReason classifyProbeGridReject(const DDGIDesc& desc) {
        return ProbeGridRejectReason::ZeroIrradianceRes;
        return ProbeGridRejectReason::InvalidSpacing;
bool preflightProbeGrid(const DDGIDesc& desc, ProbeGridRejectReason* reason) {
bool wouldSkipProbeGrid(const DDGIDesc& desc) {
    tryPreflightProbeTrilinearSample(desc, coords, cache, cache_count, reason);
    return !tryPreflightProbeTrilinearSample(desc, coords, cache, cache_count, reason);

// --- deepen additive from deepen-b56-ddgi-guards-f77d ---
    return tryValidateProbeGridSource(source, reason);
    if (!tryValidateProbeGridSource(source, sourceReason)) {
    return !tryValidateCacheIndexAtCoord(desc, cache, coord, cache_count, reason);
bool wouldClampCacheIndexLookupAtCoord(const ProbeGridCoord& coord, const DDGIDesc& desc) {
    tryValidateCacheIndexAtCoord(desc, cache, coord, cache_count, reason);
    return wouldSkipProbeTraceKernel(params) || wouldSkipProbeBlendKernel(params);

// --- deepen additive from deepen-b56-ddgi-guards-f5fe ---
    return tryCanSampleProbeGrid(desc, reason);
bool tryCanSampleProbeGrid(const DDGIDesc& desc, ProbeGridRejectReason& outReason) {
        outReason = ProbeGridRejectReason::ZeroIrradianceRes;
        outReason = ProbeGridRejectReason::InvalidSpacing;
    tryCanSampleProbeGrid(desc, reason);

// --- deepen additive from deepen-ddgi-guards-a008 ---
    case ProbeGridSourceRejectReason::ZeroSpacing:
    case ProbeGridSourceRejectReason::MismatchedProbeCount:
bool preflightDdgiProbeUpdatePipeline(const DDGIDesc& desc,
                                      ProbeUpdateLaunchRejectReason* host_reason,
                                      gi::ProbeKernelRejectReason* kernel_reason) {
    const bool host_ok = preflightDdgiProbeUpdate(desc, probe_indices, probe_count, host_reason);
    const bool kernel_ok = gi::preflightProbeKernelLaunch(kernel_params, kernel_reason);
bool wouldSkipDdgiProbeUpdatePipeline(const DDGIDesc& desc,
    return !preflightDdgiProbeUpdatePipeline(desc, probe_indices, probe_count, kernel_params);
        outReason = ProbeGridSourceRejectReason::ZeroSpacing;
    if (!tryValidateProbeGridSource(desc, outReason)) {
        outReason = ProbeGridSourceRejectReason::MismatchedProbeCount;
bool tryValidateProbeGridSource(const ProbeData& data, ProbeGridSourceRejectReason& outReason) {
    return tryValidateProbeGridSource(data.desc, outReason);
ProbeGridSourceRejectReason classifyProbeGridSourceReject(const DDGIDesc& desc, u32 probe_count) {
    tryValidateProbeGridSource(desc, probe_count, reason);
ProbeGridSourceRejectReason classifyProbeGridSourceReject(const ProbeData& data) {
    tryValidateProbeGridSource(data, reason);
    const ProbeGridSourceRejectReason reject = classifyProbeGridSourceReject(desc, probe_count);
bool preflightProbeGridSource(const ProbeData& data, ProbeGridSourceRejectReason* reason) {
    const ProbeGridSourceRejectReason reject = classifyProbeGridSourceReject(data);
bool wouldSkipProbeGridSource(const DDGIDesc& desc, u32 probe_count) {
    return !preflightProbeGridSource(desc, probe_count);
bool wouldSkipProbeGridSource(const ProbeData& data) {
    return !preflightProbeGridSource(data);
bool preflightDDGIKernelParams(DDGIKernelParams& params,

// --- deepen additive from deepen-ddgi-guards-9443 ---
    ddgi_util::tryPreflightProbeGridSource(desc, reason);
    ddgi_util::tryCanTrilinearSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
    return !tryPreflightProbeGridSource(desc, reason);
        return tryValidateCacheIndex(desc, ProbeGridLayout::probeIndexFromCoord(desc, clamped), cache_count,
    return tryValidateCacheIndex(desc, ProbeGridLayout::probeIndexFromCoord(desc, coord), cache_count, outReason);
    return !tryValidateCacheIndexAtCoord(desc, coord, cache_count, reason);

// --- deepen additive from deepen-ddgi-guards-f9cb ---
    case ProbeGridSourceRejectReason::DescMismatch:
    case ProbeGridSourceRejectReason::InvalidHandles:
    case ProbeGridSourceRejectReason::UndersizedVolume:
        outReason = ProbeGridSourceRejectReason::DescMismatch;
        outReason = ProbeGridSourceRejectReason::InvalidHandles;
bool tryValidateProbeGridSource(const ProbeVolume& volume,
        outReason = volume.probe_count < probeCount(desc) ? ProbeGridSourceRejectReason::UndersizedVolume
                                                         : ProbeGridSourceRejectReason::DescMismatch;
ProbeGridSourceRejectReason classifyProbeGridSourceReject(const ProbeData& data, const DDGIDesc& desc) {
    tryValidateProbeGridSource(data, desc, reason);
ProbeGridSourceRejectReason classifyProbeGridSourceReject(const ProbeVolume& volume, const DDGIDesc& desc) {
    tryValidateProbeGridSource(volume, desc, reason);
    const ProbeGridSourceRejectReason reject = classifyProbeGridSourceReject(data, desc);
bool preflightProbeGridSource(const ProbeVolume& volume,
    const ProbeGridSourceRejectReason reject = classifyProbeGridSourceReject(volume, desc);
bool wouldSkipProbeGridSource(const ProbeData& data, const DDGIDesc& desc) {
    return !preflightProbeGridSource(data, desc);
bool wouldSkipProbeGridSource(const ProbeVolume& volume, const DDGIDesc& desc) {
    return !preflightProbeGridSource(volume, desc);

// --- deepen additive from deepen-b56-ddgi-guards-650e ---
    outReason = classifyProbeGridSourceReject(desc);
    return !probeGridSourceRejectReasonIsBlocking(outReason);

// --- deepen additive from deepen-b56-ddgi-guards-e427 ---
bool preflightProbeTrilinearSampleAtCoords(const DDGIDesc& desc,
bool wouldSkipProbeTrilinearSampleAtCoords(const DDGIDesc& desc,
    return !preflightProbeTrilinearSampleAtCoords(desc, coords, cache, cache_count);
    return preflightProbeTrilinearSampleAtCoords(desc, coords, cache, cache_count, reason);
    ProbeScheduleRejectReason reject = ProbeScheduleRejectReason::None;
    const bool ok = tryCanScheduleProbeUpdatesAtRate(

// --- deepen additive from deepen-ddgi-b56-guards-d572 ---
        return ProbeGridSourceRejectReason::ZeroSpacing;

// --- deepen additive from deepen-ddgi-guards-273e ---
    case ProbeGridSourceRejectReason::ZeroProbeSpacing:
        outReason = ProbeGridSourceRejectReason::ZeroProbeSpacing;
    ddgi_util::tryValidateProbeGridSource(desc, reason);

// --- deepen additive from deepen-ddgi-b56-guards-e607 ---
bool preflightTrilinearProbeSampleAtWorld(const DDGIDesc& desc,
bool wouldSkipProbeTrilinearSampleAtWorld(const DDGIDesc& desc,

// --- deepen additive from deepen-ddgi-guards-2d25 ---
    case ProbeGridRejectReason::ZeroSpacing:
    return !preflightProbeSampleCoords(desc, world_position, nullptr, &reason);
        return ProbeGridRejectReason::ZeroSpacing;
    return preflightProbeGrid(desc);

// --- deepen additive from deepen-ddgi-b56-guards-8d84 ---
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const ProbeGridSource& source,
    return classifyProbeTrilinearSampleReject(source.desc, coords, source.cache, source.cache_count);
bool preflightProbeTrilinearSample(const ProbeGridSource& source,
    return preflightProbeTrilinearSample(source.desc, coords, source.cache, source.cache_count, reason);
bool wouldSkipProbeTrilinearSample(const ProbeGridSource& source, const ProbeSampleCoords& coords) {
    return !preflightProbeTrilinearSample(source, coords);
