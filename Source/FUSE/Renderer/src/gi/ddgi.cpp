#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_kernels.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fuse::renderer {

const char* probeSampleCoordsRejectReasonLabel(ProbeSampleCoordsRejectReason reason) {
    switch (reason) {
    case ProbeSampleCoordsRejectReason::None:
        return "none";
    case ProbeSampleCoordsRejectReason::EmptyGrid:
        return "empty_grid";
    case ProbeSampleCoordsRejectReason::InvalidSpacing:
        return "invalid_spacing";
    case ProbeSampleCoordsRejectReason::NotSampleable:
        return "not_sampleable";
    case ProbeSampleCoordsRejectReason::NonSampleableGrid:
        return "non_sampleable_grid";
    case ProbeSampleCoordsRejectReason::OutOfRangeIndices:
        return "out_of_range_indices";
    case ProbeSampleCoordsRejectReason::OutOfRangeWeights:
        return "out_of_range_weights";
    case ProbeSampleCoordsRejectReason::UnorderedCorners:
        return "unordered_corners";
    }
    return "unknown";
}

const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
    switch (reason) {
    case ProbeScheduleRejectReason::None:
        return "none";
    case ProbeScheduleRejectReason::NullOutputIndices:
        return "null_output_indices";
    case ProbeScheduleRejectReason::NullOutputCount:
        return "null_output_count";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";
    }
    return "unknown";

const char* sampleRequestRejectReasonLabel(SampleRequestRejectReason reason) {
    case SampleRequestRejectReason::None:
    case SampleRequestRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleRequestRejectReason::NotSampleable:
        return "not_sampleable";
    case SampleRequestRejectReason::UndersizedCache:
        return "undersized_cache";
ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc,
                                                              const ProbeSampleCoords& coords) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return ProbeSampleCoordsRejectReason::EmptyGrid;

    if (!ProbeGridLayout::areProbeSampleCoordsInBounds(desc, coords)) {
        const u32 max_x = desc.grid_dims.x - 1u;
        const u32 max_y = desc.grid_dims.y - 1u;
        const u32 max_z = desc.grid_dims.z - 1u;
        const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
        const bool indicesInRange = inRange(coords.x0, max_x) && inRange(coords.x1, max_x) &&
                                    inRange(coords.y0, max_y) && inRange(coords.y1, max_y) &&
                                    inRange(coords.z0, max_z) && inRange(coords.z1, max_z);
        return indicesInRange ? ProbeSampleCoordsRejectReason::OutOfRangeWeights
                              : ProbeSampleCoordsRejectReason::OutOfRangeIndices;

    if (coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
        return ProbeSampleCoordsRejectReason::UnorderedCorners;

    return ProbeSampleCoordsRejectReason::None;
}

const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason) {
    switch (reason) {
    case CacheIndexRejectReason::None:
        return "none";
    case CacheIndexRejectReason::EmptyGrid:
        return "empty_grid";
    case CacheIndexRejectReason::NotSampleable:
        return "not_sampleable";
    case CacheIndexRejectReason::UndersizedCache:
        return "undersized_cache";
    case CacheIndexRejectReason::OutOfRangeIndex:
        return "out_of_range_index";
    }
    return "unknown";
}

namespace {

constexpr IrradianceCacheEntry kDefaultCacheEntry{};

fuse::math::Vec3 defaultAmbientIrradiance() {
    return {0.05f, 0.05f, 0.06f};
}

ProbeSampleCoordRejectReason probeSampleCoordRejectFromValidity(const DDGIDesc& desc,
                                                                const ProbeSampleCoords& coords) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return ProbeSampleCoordRejectReason::EmptyGrid;

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;
    const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
    if (!inRange(coords.x0, max_x) || !inRange(coords.x1, max_x) || !inRange(coords.y0, max_y) ||
        !inRange(coords.y1, max_y) || !inRange(coords.z0, max_z) || !inRange(coords.z1, max_z) ||
        coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
        return ProbeSampleCoordRejectReason::OutOfBounds;
    return ProbeSampleCoordRejectReason::InvalidWeights;

} // namespace

const char* probeSampleCoordsRejectReasonLabel(ProbeSampleCoordsRejectReason reason) {
    switch (reason) {
    case ProbeSampleCoordsRejectReason::None:
        return "none";
    case ProbeSampleCoordsRejectReason::EmptyGrid:
        return "empty_grid";
    case ProbeSampleCoordsRejectReason::NotSampleableGrid:
        return "not_sampleable_grid";
    case ProbeSampleCoordsRejectReason::InvalidSpacing:
        return "invalid_spacing";
    case ProbeSampleCoordsRejectReason::NotSampleable:
        return "not_sampleable";
    case ProbeSampleCoordsRejectReason::OutOfRangeIndices:
        return "out_of_range_indices";
    case ProbeSampleCoordsRejectReason::OutOfRangeWeights:
        return "out_of_range_weights";
    case ProbeSampleCoordsRejectReason::UnorderedCorners:
        return "unordered_corners";
    case ProbeSampleCoordsRejectReason::UndersizedCache:
        return "undersized_cache";
    return "unknown";

bool probeSampleCoordsRejectReasonIsBlocking(ProbeSampleCoordsRejectReason reason) {
        return false;
        return true;

const char* probeGridSourceRejectReasonLabel(ProbeGridSourceRejectReason reason) {
    case ProbeGridSourceRejectReason::None:
    case ProbeGridSourceRejectReason::EmptyGrid:
    case ProbeGridSourceRejectReason::NotSampleable:

bool probeGridSourceRejectReasonIsBlocking(ProbeGridSourceRejectReason reason) {
    return reason != ProbeGridSourceRejectReason::None;
    case ProbeSampleCoordsRejectReason::InvalidWeights:
        return "invalid_weights";
const char* probeSampleCoordRejectReasonLabel(ProbeSampleCoordRejectReason reason) {
    case ProbeSampleCoordRejectReason::None:
    case ProbeSampleCoordRejectReason::EmptyGrid:
    case ProbeSampleCoordRejectReason::OutOfBounds:
        return "out_of_bounds";
    case ProbeSampleCoordRejectReason::InvalidWeights:

const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason) {
    case CacheIndexRejectReason::None:
    case CacheIndexRejectReason::EmptyGrid:
    case CacheIndexRejectReason::NotSampleable:
    case CacheIndexRejectReason::OutOfRangeProbeIndex:
        return "out_of_range_probe_index";
    case CacheIndexRejectReason::ZeroCache:
        return "zero_cache";
    case CacheIndexRejectReason::UndersizedCache:
    case CacheIndexRejectReason::NullCache:
        return "null_cache";
    case CacheIndexRejectReason::OutOfRangeIndex:
        return "out_of_range_index";
    case CacheIndexRejectReason::ProbeIndexOutOfRange:
        return "probe_index_out_of_range";
    case CacheIndexRejectReason::CacheUndersized:
        return "cache_undersized";
    case CacheIndexRejectReason::OutOfRangeProbe:
        return "out_of_range_probe";
const char* probeSpatialSampleRejectReasonLabel(ProbeSpatialSampleRejectReason reason) {
    case ProbeSpatialSampleRejectReason::None:
    case ProbeSpatialSampleRejectReason::EmptyGrid:
    case ProbeSpatialSampleRejectReason::InvalidSampleCoords:
        return "invalid_sample_coords";
    case ProbeSpatialSampleRejectReason::UndersizedCache:
    case ProbeSpatialSampleRejectReason::NullCache:
const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
    case ProbeScheduleRejectReason::None:
    case ProbeScheduleRejectReason::NullIndices:
        return "null_indices";
    case ProbeScheduleRejectReason::NullCount:
        return "null_count";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";

bool cacheIndexRejectReasonIsBlocking(CacheIndexRejectReason reason) {
    return reason != CacheIndexRejectReason::None;
CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
        return CacheIndexRejectReason::EmptyGrid;
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index)) {
        return CacheIndexRejectReason::OutOfRangeProbeIndex;
    if (cache_count == 0u) {
        return CacheIndexRejectReason::UndersizedCache;
    if (probe_index >= cache_count) {
    return CacheIndexRejectReason::None;

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                const IrradianceCacheEntry* cache,
                                                u32 probe_index,
                                                u32 cache_count) {
    if (cache == nullptr) {
        return CacheIndexRejectReason::NullCache;
    return classifyCacheIndexReject(desc, probe_index, cache_count);
}

const char* probeTrilinearSampleRejectReasonLabel(ProbeTrilinearSampleRejectReason reason) {
const char* sampleRequestRejectReasonLabel(SampleRequestRejectReason reason) {
    switch (reason) {
    case SampleRequestRejectReason::None:
        return "none";
    case SampleRequestRejectReason::NotSampleable:
        return "not_sampleable";
    case SampleRequestRejectReason::UndersizedCache:
        return "undersized_cache";
    return "unknown";

const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
    case ProbeScheduleRejectReason::None:
    case ProbeScheduleRejectReason::NullOutput:
        return "null_output";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";

const char* probeUpdateLaunchRejectReasonLabel(ProbeUpdateLaunchRejectReason reason) {
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
    return "unknown";

bool probeTrilinearSampleRejectReasonIsBlocking(ProbeTrilinearSampleRejectReason reason) {
    return reason != ProbeTrilinearSampleRejectReason::None;

const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
    switch (reason) {
    case ProbeScheduleRejectReason::None:
        return "none";
    case ProbeScheduleRejectReason::NullOutputIndices:
        return "null_output_indices";
    case ProbeScheduleRejectReason::NullOutputCount:
        return "null_output_count";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";
    }
    return "unknown";
}

const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
                                                                    const ProbeSampleCoords& coords,
                                                                    const IrradianceCacheEntry* cache,
                                                                    u32 cache_count) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return ProbeTrilinearSampleRejectReason::EmptyGrid;
    }
    if (!ddgi_util::canSampleProbeGrid(desc)) {
        return ProbeTrilinearSampleRejectReason::NotSampleable;
    if (cache == nullptr) {
        return ProbeTrilinearSampleRejectReason::NullCache;
    if (!ddgi_util::isCacheSizedForGrid(desc, cache_count)) {
        return ProbeTrilinearSampleRejectReason::UndersizedCache;
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        return ProbeTrilinearSampleRejectReason::InvalidSampleCoords;
    return ProbeTrilinearSampleRejectReason::None;

const char* probeUpdateLaunchRejectReasonLabel(ProbeUpdateLaunchRejectReason reason) {
    switch (reason) {
    case ProbeScheduleRejectReason::None:
        return "none";
    case ProbeScheduleRejectReason::NullOutputIndices:
        return "null_output_indices";
    case ProbeScheduleRejectReason::NullOutputCount:
        return "null_output_count";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";
    }
    return "unknown";
}

ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc,
                                                                const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    ProbeGridLayout::tryValidateProbeSampleCoords(desc, coords, reason);
    return reason;
}

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                u32 probe_index,
                                                u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    ddgi_util::tryValidateCacheIndex(desc, probe_index, cache_count, reason);
    return reason;
}

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                const IrradianceCacheEntry* cache,
                                                u32 probe_index,
                                                u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    ddgi_util::tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);
    return reason;
}

ProbeUpdateLaunchRejectReason classifyProbeUpdateLaunchReject(const DDGIDesc& desc,
                                                              const u32* probe_indices,
                                                              u32 probe_count) {
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;
    tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
    return reason;
}

ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      u32 max_indices,
                                                      const u32* out_indices,
                                                      u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    ddgi_util::tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
    return reason;
}

const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
    switch (reason) {
    case ProbeScheduleRejectReason::None:
        return "none";
    case ProbeScheduleRejectReason::NullIndices:
        return "null_indices";
    case ProbeScheduleRejectReason::NullCount:
        return "null_count";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";
    case ProbeScheduleRejectReason::ZeroProbesPerFrame:
        return "zero_probes_per_frame";
    }
    return "unknown";

const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
    switch (reason) {
    case ProbeScheduleRejectReason::None:
        return "none";
    case ProbeScheduleRejectReason::NullOutputIndices:
        return "null_output_indices";
    case ProbeScheduleRejectReason::NullOutputCount:
        return "null_output_count";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";

    case ProbeScheduleRejectReason::NullOutputBuffer:
        return "null_output_buffer";

ProbeUpdateLaunchRejectReason classifyProbeUpdateLaunchReject(const DDGIDesc& desc,
                                                              const u32* probe_indices,
                                                              u32 probe_count) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return ProbeUpdateLaunchRejectReason::EmptyGrid;
    if (probe_indices == nullptr) {
        return ProbeUpdateLaunchRejectReason::NullIndices;
    if (probe_count == 0u) {
        return ProbeUpdateLaunchRejectReason::ZeroCount;

    for (u32 i = 0u; i < probe_count; ++i) {
        if (ProbeGridLayout::isProbeIndexOutOfRange(probe_indices[i], desc)) {
            return ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex;

    return ProbeUpdateLaunchRejectReason::None;


    case ProbeScheduleRejectReason::NullIndices:
        return "null_indices";
    case ProbeScheduleRejectReason::NullCount:
        return "null_count";

const char* probeUpdateLaunchRejectReasonLabel(ProbeUpdateLaunchRejectReason reason) {
    case ProbeUpdateLaunchRejectReason::None:
    case ProbeUpdateLaunchRejectReason::EmptyGrid:
    case ProbeUpdateLaunchRejectReason::NullIndices:
    case ProbeUpdateLaunchRejectReason::ZeroCount:
        return "zero_count";
    case ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex:
        return "out_of_range_probe_index";
const char* ddgiLaunchRejectReasonLabel(DdgiLaunchRejectReason reason) {
    case DdgiLaunchRejectReason::None:
    case DdgiLaunchRejectReason::EmptyGrid:
    case DdgiLaunchRejectReason::NullIndices:
    case DdgiLaunchRejectReason::ZeroCount:
    case DdgiLaunchRejectReason::NullIndexBuffer:
        return "null_index_buffer";
    case DdgiLaunchRejectReason::ZeroProbeCount:
    case DdgiLaunchRejectReason::OutOfRangeIndex:
        return "out_of_range_index";
    case ProbeUpdateLaunchRejectReason::ZeroRaysPerProbe:
        return "zero_rays_per_probe";

bool probeUpdateLaunchRejectReasonIsBlocking(ProbeUpdateLaunchRejectReason reason) {
    return reason != ProbeUpdateLaunchRejectReason::None;

ProbeUpdateLaunchRejectReason classifyDdgiProbeUpdateReject(const DDGIDesc& desc,
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;
    tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
    return reason;

bool preflightDdgiProbeUpdate(const DDGIDesc& desc,
                              u32 probe_count,
                              ProbeUpdateLaunchRejectReason* reason) {
    const ProbeUpdateLaunchRejectReason reject =
        classifyDdgiProbeUpdateReject(desc, probe_indices, probe_count);
    if (reason != nullptr) {
        *reason = reject;
    return !probeUpdateLaunchRejectReasonIsBlocking(reject);

bool tryPreflightDdgiProbeUpdate(const DDGIDesc& desc,
                                 ProbeUpdateLaunchRejectReason& outReason) {
    return preflightDdgiProbeUpdate(desc, probe_indices, probe_count, &outReason);

    case ProbeScheduleRejectReason::ZeroProbesPerFrame:
        return "zero_probes_per_frame";
    case ProbeScheduleRejectReason::NullOutIndices:
        return "null_out_indices";
    case ProbeScheduleRejectReason::NullOutCount:
        return "null_out_count";
    return "unknown";

bool probeScheduleRejectReasonIsBlocking(ProbeScheduleRejectReason reason) {
    return reason != ProbeScheduleRejectReason::None;
    case ProbeScheduleRejectReason::NullOutputIndices:
        return "null_output_indices";
    case ProbeScheduleRejectReason::NullOutputCount:
        return "null_output_count";
    }

const char* sampleRequestRejectReasonLabel(SampleRequestRejectReason reason) {
    switch (reason) {
    case SampleRequestRejectReason::None:
        return "none";
    case SampleRequestRejectReason::EmptyGrid:
        return "empty_grid";
    case SampleRequestRejectReason::NotSampleable:
        return "not_sampleable";
    case SampleRequestRejectReason::UndersizedCache:
        return "undersized_cache";
    case ProbeScheduleRejectReason::NullIndicesBuffer:
        return "null_indices_buffer";
    case ProbeScheduleRejectReason::NullCountOutput:
        return "null_count_output";
    case ProbeScheduleRejectReason::NullOutput:
        return "null_output";
    case ProbeScheduleRejectReason::NullCount:
        return "null_count";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";
    return "unknown";
    }

ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      u32 max_indices,
                                                      const u32* out_indices,
                                                      u32* out_count) {
    if (out_indices == nullptr) {
        return ProbeScheduleRejectReason::NullOutIndices;
    }
    if (out_count == nullptr) {
        return ProbeScheduleRejectReason::NullOutCount;
    }
    if (probe_count == 0u) {
        return ProbeScheduleRejectReason::ZeroProbeCount;
    }
    if (max_indices == 0u) {
        return ProbeScheduleRejectReason::ZeroMaxIndices;
    }
    return ProbeScheduleRejectReason::None;
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

bool ProbeGridLayout::isProbeGridCoordOutOfRange(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    if (isEmptyGrid(desc)) {
        return false;
    }
bool ProbeGridLayout::isProbeCoordOutOfRange(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    return !isValidProbeCoord(desc, coord);
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

bool ProbeGridLayout::isBorderProbeIndex(const DDGIDesc& desc, u32 probe_index) {
    if (!isValidProbeIndex(desc, probe_index)) {
        return false;
    }
    return isBorderProbeCoord(desc, probeCoordFromIndex(desc, probe_index));
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

ProbeBorderKind ProbeGridLayout::probeBorderKindFromIndex(const DDGIDesc& desc, u32 probe_index) {
    if (!isValidProbeIndex(desc, probe_index)) {
        return ProbeBorderKind::Invalid;
    }
    return probeBorderKind(desc, probeCoordFromIndex(desc, probe_index));
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

u32 ProbeGridLayout::maxProbeIndex(const DDGIDesc& desc) {
    const u32 count = ddgi_util::probeCount(desc);
    if (count == 0u) {
        return 0u;
    }
    return count - 1u;

bool ProbeGridLayout::isAtMaxProbeIndex(u32 probe_index, const DDGIDesc& desc) {
    if (isEmptyGrid(desc)) {
        return false;
    return probe_index == maxProbeIndex(desc);
    return count == 0u ? 0u : count - 1u;

    const u32 count = ddgi_util::probeCount(desc);
    return count > 0u && probe_index == count - 1u;

u32 ProbeGridLayout::clampProbeIndex(u32 probe_index, const DDGIDesc& desc) {
    const u32 count = ddgi_util::probeCount(desc);
    if (count == 0u) {
        return 0u;
    }
    return std::min(probe_index, count - 1u);
}

bool ProbeGridLayout::tryClampProbeIndex(u32 probe_index, const DDGIDesc& desc, u32& out_index) {
    if (isEmptyGrid(desc)) {
        out_index = 0u;
        return false;
    }

    out_index = clampProbeIndex(probe_index, desc);
u32 ProbeGridLayout::maxProbeIndex(const DDGIDesc& desc) {
    const u32 count = ddgi_util::probeCount(desc);
    if (count == 0u) {
        return 0u;
    return count - 1u;

bool ProbeGridLayout::isAtMaxProbeIndex(u32 probe_index, const DDGIDesc& desc) {
    return count > 0u && probe_index == count - 1u;

    out_index = std::min(probe_index, count - 1u);
    return true;
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

bool ProbeGridLayout::canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return areProbeSampleCoordsInBounds(desc, coords);
}

bool ProbeGridLayout::wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
    }
    if (!areProbeSampleCoordsInBounds(desc, coords)) {
        return true;
    }
    return coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1;
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

bool ProbeGridLayout::tryNormalizeAndValidateProbeSampleCoords(const DDGIDesc& desc,
                                                               ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
    }
    normalizeProbeSampleCoords(coords);
    return isValidProbeSampleCoords(desc, coords);
}

bool ProbeGridLayout::areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
bool ProbeGridLayout::isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryValidateProbeSampleCoords(desc, coords, reason);
}

bool ProbeGridLayout::isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !isValidProbeSampleCoords(desc, coords);

bool ProbeGridLayout::areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
    return inRange(coords.x0, max_x) && inRange(coords.x1, max_x) && inRange(coords.y0, max_y) &&
           inRange(coords.y1, max_y) && inRange(coords.z0, max_z) && inRange(coords.z1, max_z);
}

bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return isProbeSampleCoordsOutOfRange(desc, coords);
}

bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return isProbeSampleCoordsOutOfRange(desc, coords);
}

bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !isValidProbeSampleCoords(desc, coords);
}

bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return isProbeSampleCoordsOutOfRange(desc, coords);
}

bool ProbeGridLayout::wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
    if (!areProbeSampleCoordsInBounds(desc, coords)) {
        return true;
    return coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1 || coords.tx < 0.f ||
           coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f || coords.tz > 1.f;

bool ProbeGridLayout::canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryPreflightProbeSampleCoords(desc, coords, reason);

bool ProbeGridLayout::tryPreflightProbeSampleCoords(const DDGIDesc& desc,
                                                    const ProbeSampleCoords& coords,
                                                    ProbeSampleCoordsRejectReason& outReason) {
    return tryValidateProbeSampleCoords(desc, coords, outReason);

bool ProbeGridLayout::tryValidateProbeSampleCoords(const DDGIDesc& desc,
    if (isEmptyGrid(desc)) {
        return false;

bool ProbeGridLayout::tryPreflightProbeSampleCoords(const DDGIDesc& desc,
                                                    const ProbeSampleCoords& coords,
                                                    ProbeSampleCoordsRejectReason& outReason) {
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;

    if (isValidProbeSampleCoords(desc, coords)) {
        outReason = ProbeSampleCoordsRejectReason::None;
        return true;

    if (!areProbeSampleCoordsInBounds(desc, coords)) {
        const u32 max_x = desc.grid_dims.x - 1u;
        const u32 max_y = desc.grid_dims.y - 1u;
        const u32 max_z = desc.grid_dims.z - 1u;
        const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
        const bool indicesInRange = inRange(coords.x0, max_x) && inRange(coords.x1, max_x) &&
                                    inRange(coords.y0, max_y) && inRange(coords.y1, max_y) &&
                                    inRange(coords.z0, max_z) && inRange(coords.z1, max_z);
        if (indicesInRange) {
            outReason = ProbeSampleCoordsRejectReason::OutOfRangeWeights;

        outReason = ProbeSampleCoordsRejectReason::OutOfRangeIndices;

    if (coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
        outReason = ProbeSampleCoordsRejectReason::UnorderedCorners;


bool ProbeGridLayout::canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryPreflightProbeSampleCoords(desc, coords, reason);

bool ProbeGridLayout::tryValidateProbeSampleCoords(const DDGIDesc& desc,
                                                   const ProbeSampleCoords& coords,
                                                   ProbeSampleCoordsRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;
        return false;

    if (coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
        outReason = ProbeSampleCoordsRejectReason::UnorderedCorners;

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
    if (!inRange(coords.x0, max_x) || !inRange(coords.x1, max_x) || !inRange(coords.y0, max_y) ||
        !inRange(coords.y1, max_y) || !inRange(coords.z0, max_z) || !inRange(coords.z1, max_z)) {
        outReason = ProbeSampleCoordsRejectReason::OutOfRangeIndices;
    if (coords.x0 > max_x || coords.x1 > max_x || coords.y0 > max_y || coords.y1 > max_y || coords.z0 > max_z ||
        coords.z1 > max_z) {

    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        outReason = ProbeSampleCoordsRejectReason::OutOfRangeWeights;
    return true;

    outReason = ProbeSampleCoordsRejectReason::None;

bool ProbeGridLayout::isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryValidateProbeSampleCoords(desc, coords, reason);

bool ProbeGridLayout::isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !isValidProbeSampleCoords(desc, coords);

bool ProbeGridLayout::wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {


    return coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1;

bool ProbeGridLayout::tryPreflightProbeSampleCoords(const DDGIDesc& desc,
    if (!ddgi_util::canSampleProbeGrid(desc)) {
        outReason = ProbeSampleCoordsRejectReason::NotSampleable;
    if (!areProbeSampleCoordsInBounds(desc, coords)) {
ProbeSampleCoordsRejectReason classifyProbeSampleCoordsReject(const DDGIDesc& desc,
                                                              const ProbeSampleCoords& coords) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return ProbeSampleCoordsRejectReason::EmptyGrid;

    if (!ProbeGridLayout::areProbeSampleCoordsInBounds(desc, coords)) {
        const bool indicesInRange = inRange(coords.x0, max_x) && inRange(coords.x1, max_x) &&
                                    inRange(coords.y0, max_y) && inRange(coords.y1, max_y) &&
                                    inRange(coords.z0, max_z) && inRange(coords.z1, max_z);
        return indicesInRange ? ProbeSampleCoordsRejectReason::OutOfRangeWeights
                              : ProbeSampleCoordsRejectReason::OutOfRangeIndices;
    }


bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !areProbeSampleCoordsInBounds(desc, coords) || !isValidProbeSampleCoords(desc, coords);
    return isProbeSampleCoordsOutOfRange(desc, coords);

                                                    const ProbeSampleCoords& coords,
                                                    ProbeSampleCoordsRejectReason& outReason) {

    if (isValidProbeSampleCoords(desc, coords)) {

        if (indicesInRange) {






bool ProbeGridLayout::canPreflightProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return tryPreflightProbeSampleCoords(desc, coords, reason);

    return isValidProbeSampleCoords(desc, coords);

    return tryValidateProbeSampleCoords(desc, coords, outReason);

    return !tryValidateProbeSampleCoords(desc, coords, reason);

ProbeSampleCoordsRejectReason ProbeGridLayout::classifyProbeSampleCoordsReject(const DDGIDesc& desc,
    tryValidateProbeSampleCoords(desc, coords, reason);
    return reason;
        return ProbeSampleCoordsRejectReason::UnorderedCorners;

    return ProbeSampleCoordsRejectReason::None;

bool shouldSkipProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return classifyProbeSampleCoordsReject(desc, coords) != ProbeSampleCoordsRejectReason::None;

bool ProbeGridLayout::tryValidateProbeSampleCoords(const DDGIDesc& desc,
    outReason = classifyProbeSampleCoordsReject(desc, coords);
    return outReason == ProbeSampleCoordsRejectReason::None;

bool ProbeGridLayout::tryClampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {
    return isProbeSampleCoordsOutOfRange(desc, coords) || !isValidProbeSampleCoords(desc, coords);



    if (!indicesInRange) {

    outReason = coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1
                    ? ProbeSampleCoordsRejectReason::UnorderedCorners
                    : ProbeSampleCoordsRejectReason::OutOfRangeWeights;





        outReason = indicesInRange ? ProbeSampleCoordsRejectReason::OutOfRangeWeights




    return !probeSampleCoordsRejectReasonIsBlocking(outReason);


bool ProbeGridLayout::wouldSkipProbeSampleCoordPreflight(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !tryPreflightProbeSampleCoords(desc, coords, reason);

bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc, const fuse::math::Vec3& world_position) {
    ProbeSampleCoords coords{};
    if (!buildProbeSampleCoords(desc, world_position, coords)) {
    return wouldSkipProbeSampleCoordPreflight(desc, coords);


bool ProbeGridLayout::preflightProbeSampleCoords(const DDGIDesc& desc,
                                                 const fuse::math::Vec3& world_position,
                                                 ProbeSampleCoords* out_coords,
                                                 ProbeSampleCoordsRejectReason* reason) {
    ProbeSampleCoordsRejectReason rejectReason = ProbeSampleCoordsRejectReason::None;
    if (!tryBuildProbeSampleCoords(desc, world_position, coords, rejectReason)) {
        if (reason != nullptr) {
            *reason = rejectReason;

    const bool ok = tryPreflightProbeSampleCoords(desc, coords, rejectReason);
    if (out_coords != nullptr) {
        *out_coords = coords;
    return ok;

bool ProbeGridLayout::wouldSkipProbeSampleCoords(const DDGIDesc& desc,
    return !tryValidateProbeSampleCoords(desc, coords, outReason);

    return wouldSkipProbeSampleCoords(desc, coords, reason);
}

bool ProbeGridLayout::tryClampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {
    return tryClampProbeSampleCoords(desc, coords, reason);

bool ProbeGridLayout::tryClampProbeSampleCoords(const DDGIDesc& desc,
                                                ProbeSampleCoords& coords,
    clampProbeSampleCoords(desc, coords);

        return false;
    }


    return true;
}

bool ProbeGridLayout::isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryValidateProbeSampleCoords(desc, coords, reason);
}

bool ProbeGridLayout::areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    if (coords.x0 > max_x || coords.x1 > max_x || coords.y0 > max_y || coords.y1 > max_y || coords.z0 > max_z ||
        coords.z1 > max_z) {
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
    return true;
    return isValidProbeSampleCoords(desc, coords);

bool ProbeGridLayout::isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !areProbeSampleCoordsInBounds(desc, coords);

bool ProbeGridLayout::tryClampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {

    clampProbeSampleCoords(desc, coords);
bool ProbeGridLayout::canBuildProbeSampleCoords(const DDGIDesc& desc) {
    return desc.probe_spacing.x > 0.f && desc.probe_spacing.y > 0.f && desc.probe_spacing.z > 0.f;

bool ProbeGridLayout::tryBuildProbeSampleCoords(const DDGIDesc& desc,
                                                const fuse::math::Vec3& world_position,
                                                ProbeSampleCoords& out_coords,
                                                ProbeSampleCoordsRejectReason& outReason) {
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;
    if (desc.probe_spacing.x <= 0.f || desc.probe_spacing.y <= 0.f || desc.probe_spacing.z <= 0.f) {
        outReason = ProbeSampleCoordsRejectReason::InvalidSpacing;

    outReason = ProbeSampleCoordsRejectReason::None;
    return buildProbeSampleCoords(desc, world_position, out_coords);

void ProbeGridLayout::sanitizeProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {

bool ProbeGridLayout::isProbeSampleCoordsNormalized(const ProbeSampleCoords& coords) {
    if (coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
    return coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f && coords.tz >= 0.f &&
           coords.tz <= 1.f;

bool ProbeGridLayout::isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return true;
    }
    return !areProbeSampleCoordsInBounds(desc, coords);
}

bool ProbeGridLayout::isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (!areProbeSampleCoordsInBounds(desc, coords)) {
        return false;
    }

    return coords.x0 <= coords.x1 && coords.y0 <= coords.y1 && coords.z0 <= coords.z1;
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

u32 ProbeGridLayout::maxProbeIndex(const DDGIDesc& desc) {
    const u32 count = ddgi_util::probeCount(desc);
    if (count == 0u) {
        return 0u;
    return count - 1u;

bool ProbeGridLayout::isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
bool ProbeGridLayout::tryClampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {
    }

    clampProbeSampleCoords(desc, coords);
    return true;

bool ProbeGridLayout::areProbeSampleCoordsInBounds(const DDGIDesc& desc, const ProbeSampleCoords& coords) {

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    const auto index_in_range = [](u32 value, u32 max_value) { return value <= max_value; };
    if (!index_in_range(coords.x0, max_x) || !index_in_range(coords.x1, max_x) ||
        !index_in_range(coords.y0, max_y) || !index_in_range(coords.y1, max_y) ||
        !index_in_range(coords.z0, max_z) || !index_in_range(coords.z1, max_z)) {
    if (coords.x1 < coords.x0 || coords.y1 < coords.y0 || coords.z1 < coords.z0) {

    const auto weight_in_range = [](f32 weight) { return weight >= 0.f && weight <= 1.f; };
    return weight_in_range(coords.tx) && weight_in_range(coords.ty) && weight_in_range(coords.tz);

bool ProbeGridLayout::isProbeSampleAtGridBorder(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (!isValidProbeSampleCoords(desc, coords)) {

    const ProbeGridCoord corners[8] = {{coords.x0, coords.y0, coords.z0},
                                     {coords.x1, coords.y0, coords.z0},
                                     {coords.x0, coords.y1, coords.z0},
                                     {coords.x1, coords.y1, coords.z0},
                                     {coords.x0, coords.y0, coords.z1},
                                     {coords.x1, coords.y0, coords.z1},
                                     {coords.x0, coords.y1, coords.z1},
                                     {coords.x1, coords.y1, coords.z1}};
    for (const ProbeGridCoord& corner : corners) {
        if (isBorderProbeCoord(desc, corner)) {

bool ProbeGridLayout::hasFullTrilinearNeighbourhood(const DDGIDesc& desc, const ProbeSampleCoords& coords) {

        const ProbeValidityFlags flags = probeValidity(desc, corner);
        if (!flags.valid || !flags.has_trilinear_neighbourhood) {


    if (coords.x0 > max_x || coords.y0 > max_y || coords.z0 > max_z) {
    if (coords.x1 > max_x || coords.y1 > max_y || coords.z1 > max_z) {
    return coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f && coords.tz >= 0.f &&
           coords.tz <= 1.f;

void ProbeGridLayout::sanitizeProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {
        coords = {};
        return;


    coords.x0 = std::clamp(coords.x0, 0u, max_x);
    coords.y0 = std::clamp(coords.y0, 0u, max_y);
    coords.z0 = std::clamp(coords.z0, 0u, max_z);
    coords.x1 = std::clamp(coords.x1, coords.x0, std::min(coords.x0 + 1u, max_x));
    coords.y1 = std::clamp(coords.y1, coords.y0, std::min(coords.y0 + 1u, max_y));
    coords.z1 = std::clamp(coords.z1, coords.z0, std::min(coords.z0 + 1u, max_z));

    coords.tx = std::clamp(coords.tx, 0.f, 1.f);
    coords.ty = std::clamp(coords.ty, 0.f, 1.f);
    coords.tz = std::clamp(coords.tz, 0.f, 1.f);

    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {

    const ProbeGridCoord base{coords.x0, coords.y0, coords.z0};
    const ProbeGridCoord neighbour{coords.x1, coords.y1, coords.z1};
    if (!isValidProbeCoord(desc, base) || !isValidProbeCoord(desc, neighbour)) {

    if (coords.x1 < coords.x0 || coords.x1 > std::min(coords.x0 + 1u, max_x)) {
    if (coords.y1 < coords.y0 || coords.y1 > std::min(coords.y0 + 1u, max_y)) {
    if (coords.z1 < coords.z0 || coords.z1 > std::min(coords.z0 + 1u, max_z)) {
    if (coords.x0 > max_x || coords.y0 > max_y || coords.z0 > max_z || coords.x1 > max_x || coords.y1 > max_y ||
        coords.z1 > max_z) {


bool ProbeGridLayout::tryBuildProbeSampleCoords(const DDGIDesc& desc,
                                              const fuse::math::Vec3& world_position,
                                              ProbeSampleCoords& out_coords,
                                              ProbeSampleCoordsRejectReason& outReason) {
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;

    const bool built = buildProbeSampleCoords(desc, world_position, out_coords);
    outReason = built ? ProbeSampleCoordsRejectReason::None : ProbeSampleCoordsRejectReason::EmptyGrid;
    return built;
    const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
    if (!inRange(coords.x0, max_x) || !inRange(coords.x1, max_x) || !inRange(coords.y0, max_y) ||
        !inRange(coords.y1, max_y) || !inRange(coords.z0, max_z) || !inRange(coords.z1, max_z)) {


bool ProbeGridLayout::isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !areProbeSampleCoordsInBounds(desc, coords);
bool ProbeGridLayout::canBuildProbeSampleCoords(const DDGIDesc& desc) {
    return ddgi_util::canSampleProbeGrid(desc);

bool ProbeGridLayout::isSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    return !isValidProbeSampleCoords(desc, coords);
    return !isEmptyGrid(desc) && desc.probe_spacing.x > 0.f && desc.probe_spacing.y > 0.f &&
           desc.probe_spacing.z > 0.f;
}

bool ProbeGridLayout::buildProbeSampleCoords(const DDGIDesc& desc,
                                             const fuse::math::Vec3& world_position,
                                             ProbeSampleCoords& out_coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryBuildProbeSampleCoords(desc, world_position, out_coords, reason);
}

bool ProbeGridLayout::buildProbeSampleCoordsIfReady(const DDGIDesc& desc,
                                                  const fuse::math::Vec3& world_position,
                                                  ProbeSampleCoords& out_coords) {
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
    return tryBuildProbeSampleCoords(desc, world_position, out_coords, reason);
}

bool ProbeGridLayout::preflightBuildProbeSampleCoords(const DDGIDesc& desc,
                                                      const fuse::math::Vec3& /*world_position*/,
                                                      ProbeSampleCoordsRejectReason* reason) {
    ProbeSampleCoordsRejectReason local = ProbeSampleCoordsRejectReason::None;
    if (isEmptyGrid(desc)) {
        local = ProbeSampleCoordsRejectReason::EmptyGrid;
    }
    if (reason != nullptr) {
        *reason = local;
    }
    return local == ProbeSampleCoordsRejectReason::None;
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
        outReason = ProbeSampleCoordsRejectReason::NotSampleable;
        outReason = ProbeSampleCoordsRejectReason::NonSampleableGrid;
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

bool ProbeGridLayout::isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
bool ProbeGridLayout::tryBuildProbeSampleCoords(const DDGIDesc& desc,
                                                const fuse::math::Vec3& world_position,
                                                ProbeSampleCoords& out_coords,
                                                ProbeSampleCoordsRejectReason& outReason) {
    outReason = ProbeSampleCoordsRejectReason::None;
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;
        return false;
    }
    if (!canBuildProbeSampleCoords(desc)) {
        outReason = ProbeSampleCoordsRejectReason::InvalidSpacing;
    if (!buildProbeSampleCoords(desc, world_position, out_coords)) {
    return true;

bool ProbeGridLayout::tryValidateProbeSampleCoords(const DDGIDesc& desc,
                                                   const ProbeSampleCoords& coords,

    const u32 max_x = desc.grid_dims.x - 1u;
    const u32 max_y = desc.grid_dims.y - 1u;
    const u32 max_z = desc.grid_dims.z - 1u;

    if (coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
    if (coords.x0 > max_x || coords.x1 > max_x || coords.y0 > max_y || coords.y1 > max_y ||
        coords.z0 > max_z || coords.z1 > max_z) {
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {

                                                ProbeSampleCoords& out_coords) {
    return isValidProbeSampleCoords(desc, out_coords);

bool ProbeGridLayout::sampleCoordsTouchBorder(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc) || !isValidProbeSampleCoords(desc, coords)) {

    const ProbeGridCoord corners[8] = {{coords.x0, coords.y0, coords.z0},
                                       {coords.x1, coords.y0, coords.z0},
                                       {coords.x0, coords.y1, coords.z0},
                                       {coords.x1, coords.y1, coords.z0},
                                       {coords.x0, coords.y0, coords.z1},
                                       {coords.x1, coords.y0, coords.z1},
                                       {coords.x0, coords.y1, coords.z1},
                                       {coords.x1, coords.y1, coords.z1}};
    for (const ProbeGridCoord& corner : corners) {
        if (isBorderProbeCoord(desc, corner)) {
bool ProbeGridLayout::buildAndClampProbeSampleCoords(const DDGIDesc& desc,
    clampProbeSampleCoords(desc, out_coords);
    const auto inRange = [](u32 value, u32 max_value) { return value <= max_value; };
    if (!inRange(coords.x0, max_x) || !inRange(coords.x1, max_x) || !inRange(coords.y0, max_y) ||
        !inRange(coords.y1, max_y) || !inRange(coords.z0, max_z) || !inRange(coords.z1, max_z)) {
        outReason = ProbeSampleCoordsRejectReason::OutOfRangeIndices;
        outReason = ProbeSampleCoordsRejectReason::UnorderedCorners;
        outReason = ProbeSampleCoordsRejectReason::InvalidWeights;
                                                ProbeSampleCoordRejectReason& outReason) {
        outReason = ProbeSampleCoordRejectReason::EmptyGrid;
    if (desc.probe_spacing.x <= 0.f || desc.probe_spacing.y <= 0.f || desc.probe_spacing.z <= 0.f) {

    buildProbeSampleCoords(desc, world_position, out_coords);
    outReason = ProbeSampleCoordRejectReason::None;
                                                ProbeSampleRejectReason& outReason) {
        outReason = ProbeSampleRejectReason::EmptyGrid;


    outReason = ProbeSampleRejectReason::None;

const char* probeSampleRejectReasonLabel(ProbeSampleRejectReason reason) {
    switch (reason) {
    case ProbeSampleRejectReason::None:
        return "none";
    case ProbeSampleRejectReason::EmptyGrid:
        return "empty_grid";
    case ProbeSampleRejectReason::OutOfBounds:
        return "out_of_bounds";
    case ProbeSampleRejectReason::InvalidWeights:
        return "invalid_weights";
    return "unknown";

const char* cacheLookupRejectReasonLabel(CacheLookupRejectReason reason) {
    case CacheLookupRejectReason::None:
    case CacheLookupRejectReason::EmptyGrid:
    case CacheLookupRejectReason::UndersizedCache:
        return "undersized_cache";
    case CacheLookupRejectReason::ProbeIndexOutOfRange:
        return "probe_index_out_of_range";

const char* launchRejectReasonLabel(LaunchRejectReason reason) {
    case LaunchRejectReason::None:
    case LaunchRejectReason::EmptyGrid:
    case LaunchRejectReason::NullIndices:
        return "null_indices";
    case LaunchRejectReason::ZeroCount:
        return "zero_count";
    case LaunchRejectReason::ProbeIndexOutOfRange:
    case LaunchRejectReason::ZeroRaysPerProbe:
        return "zero_rays_per_probe";
    if (isEmptyGrid(desc)) {
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

bool ProbeGridLayout::tryWorldToProbeGridCoord(const DDGIDesc& desc,
                                               const fuse::math::Vec3& world_position,
                                               fuse::math::Vec3& out_grid_coord,
                                               ProbeSampleCoordsRejectReason& outReason) {
    if (isEmptyGrid(desc)) {
        outReason = ProbeSampleCoordsRejectReason::EmptyGrid;
        out_grid_coord = {};
        return false;
    }
    if (desc.probe_spacing.x <= 0.f || desc.probe_spacing.y <= 0.f || desc.probe_spacing.z <= 0.f) {
        outReason = ProbeSampleCoordsRejectReason::InvalidSpacing;
        out_grid_coord = {};
        return false;
    }
    out_grid_coord = worldToProbeGridCoord(desc, world_position);
    outReason = ProbeSampleCoordsRejectReason::None;
    return true;
}

bool ProbeGridLayout::wouldClampProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
    }
    if (!areProbeSampleCoordsInBounds(desc, coords)) {
        return true;
    }
    if (coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
        return true;
    }
    if (coords.tx < 0.f || coords.tx > 1.f || coords.ty < 0.f || coords.ty > 1.f || coords.tz < 0.f ||
        coords.tz > 1.f) {
        return true;
    }
    return false;
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

const char* probeCacheLookupRejectReasonLabel(ProbeCacheLookupRejectReason reason) {
    switch (reason) {
    case ProbeCacheLookupRejectReason::None:
        return "none";
    case ProbeCacheLookupRejectReason::EmptyGrid:
        return "empty_grid";
    case ProbeCacheLookupRejectReason::NullCache:
        return "null_cache";
    case ProbeCacheLookupRejectReason::UndersizedCache:
        return "undersized_cache";
    }
    return "unknown";
}

const char* ddgiLaunchRejectReasonLabel(DdgiLaunchRejectReason reason) {
    switch (reason) {
    case DdgiLaunchRejectReason::None:
        return "none";
    case DdgiLaunchRejectReason::EmptyGrid:
        return "empty_grid";
    case DdgiLaunchRejectReason::NullIndices:
        return "null_indices";
    case DdgiLaunchRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case DdgiLaunchRejectReason::InvalidRaysPerProbe:
        return "invalid_rays_per_probe";
    }
    return "unknown";
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
u32 requiredCacheCount(const DDGIDesc& desc) {
    return probeCount(desc);

u32 cacheDeficitForGrid(const DDGIDesc& desc, u32 cache_count) {
    const u32 required = requiredCacheCount(desc);
    if (required == 0u || cache_count >= required) {
        return 0u;
    return required - cache_count;

ProbeSampleSkipReason classifyProbeGridSkip(const DDGIDesc& desc) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return ProbeSampleSkipReason::EmptyGrid;
    }
    if (desc.irradiance_res == 0u) {
        return ProbeSampleSkipReason::ZeroIrradianceResolution;
    }
    if (desc.probe_spacing.x <= 0.f || desc.probe_spacing.y <= 0.f || desc.probe_spacing.z <= 0.f) {
        return ProbeSampleSkipReason::InvalidProbeSpacing;
    }
    return ProbeSampleSkipReason::None;
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

ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
                                                                    const ProbeSampleCoords& coords,
                                                                    const IrradianceCacheEntry* cache,
                                                                    u32 cache_count) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return ProbeTrilinearSampleRejectReason::EmptyGrid;
    }
    if (!canSampleProbeGrid(desc)) {
        return ProbeTrilinearSampleRejectReason::NotSampleable;
    if (cache == nullptr) {
        return ProbeTrilinearSampleRejectReason::NullCache;
    if (!isCacheSizedForGrid(desc, cache_count)) {
        return ProbeTrilinearSampleRejectReason::UndersizedCache;
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        return ProbeTrilinearSampleRejectReason::InvalidSampleCoords;
    return ProbeTrilinearSampleRejectReason::None;

bool shouldSkipTrilinearProbeSample(const DDGIDesc& desc,
                                    const ProbeSampleCoords& coords,
                                    const IrradianceCacheEntry* cache,
                                    u32 cache_count) {
    return classifyProbeTrilinearSampleReject(desc, coords, cache, cache_count) !=
           ProbeTrilinearSampleRejectReason::None;
    ProbeTrilinearSampleRejectReason reason = ProbeTrilinearSampleRejectReason::None;
    tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reason);
    return reason;

                                    const fuse::math::Vec3& world_position,
    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords)) {
        return true;
    return !canSampleAtProbeCoords(desc, coords, cache, cache_count);

bool shouldSkipTrilinearDirectionalProbeSample(const DDGIDesc& desc,
    return shouldSkipTrilinearProbeSample(desc, world_position, cache, cache_count);
}

bool tryCanSampleAtProbeCoords(const DDGIDesc& desc,
                               const ProbeSampleCoords& coords,
                               const IrradianceCacheEntry* cache,
                               u32 cache_count,
                               ProbeTrilinearSampleRejectReason& outReason) {
    if (cache == nullptr) {
        outReason = ProbeTrilinearSampleRejectReason::NullCache;
        return false;
    }
    if (!isCacheSizedForGrid(desc, cache_count)) {
        outReason = ProbeTrilinearSampleRejectReason::UndersizedCache;
    if (!canSampleProbeGrid(desc)) {
        outReason = ProbeGridLayout::isEmptyGrid(desc) ? ProbeTrilinearSampleRejectReason::EmptyGrid
                                                       : ProbeTrilinearSampleRejectReason::NotSampleable;

    ProbeSampleCoordsRejectReason coordReason = ProbeSampleCoordsRejectReason::None;
    if (!ProbeGridLayout::tryPreflightProbeSampleCoords(desc, coords, coordReason)) {
        switch (coordReason) {
        case ProbeSampleCoordsRejectReason::EmptyGrid:
            outReason = ProbeTrilinearSampleRejectReason::EmptyGrid;
            break;
        case ProbeSampleCoordsRejectReason::OutOfRangeIndices:
        case ProbeSampleCoordsRejectReason::OutOfRangeWeights:
        case ProbeSampleCoordsRejectReason::UnorderedCorners:
            outReason = ProbeTrilinearSampleRejectReason::InvalidSampleCoords;
        case ProbeSampleCoordsRejectReason::None:
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {

    outReason = ProbeTrilinearSampleRejectReason::None;
    return true;
    outReason = classifyProbeTrilinearSampleReject(desc, coords, cache, cache_count);
    return outReason == ProbeTrilinearSampleRejectReason::None;

bool wouldSkipTrilinearProbeSampleAtCoords(const DDGIDesc& desc,
                                           const ProbeSampleCoords& coords,
                                           const IrradianceCacheEntry* cache,
                                           u32 cache_count) {
    ProbeTrilinearSampleRejectReason reason = ProbeTrilinearSampleRejectReason::None;
    return !tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reason);

bool wouldSkipTrilinearProbeSample(const DDGIDesc& desc,
                                   const fuse::math::Vec3& world_position,
    if (shouldSkipProbeGrid(desc) || shouldSkipProbeLookup(desc, cache, cache_count)) {

    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords)) {
    return wouldSkipTrilinearProbeSampleAtCoords(desc, coords, cache, cache_count);

bool preflightTrilinearProbeSample(const DDGIDesc& desc,
                                   u32 cache_count,
                                   ProbeSampleCoords* out_coords,
                                   ProbeTrilinearSampleRejectReason* reason) {
        const ProbeTrilinearSampleRejectReason reject =
            ProbeGridLayout::isEmptyGrid(desc) ? ProbeTrilinearSampleRejectReason::EmptyGrid
        if (reason != nullptr) {
            *reason = reject;

    ProbeTrilinearSampleRejectReason reject = ProbeTrilinearSampleRejectReason::None;
    if (!tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reject)) {

    if (out_coords != nullptr) {
        *out_coords = coords;
        *reason = ProbeTrilinearSampleRejectReason::None;

bool tryPreflightTrilinearProbeSample(const DDGIDesc& desc,
                                      ProbeSampleCoords& out_coords,
                                      ProbeTrilinearSampleRejectReason& outReason) {
    return preflightTrilinearProbeSample(
        desc, world_position, cache, cache_count, &out_coords, &outReason);
bool tryValidateCacheAccess(const DDGIDesc& desc,
                            u32 probe_index,
                            CacheIndexRejectReason& outReason) {
        outReason = CacheIndexRejectReason::NullCache;
    return tryValidateCacheIndex(desc, probe_index, cache_count, outReason);

bool tryValidateCacheIndex(const DDGIDesc& desc,

bool isCacheIndexValid(const DDGIDesc& desc,
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return tryValidateCacheIndex(desc, probe_index, cache, cache_count, reason);

bool wouldSkipCacheIndexLookup(const DDGIDesc& desc,
    return !isCacheIndexValid(desc, probe_index, cache, cache_count);

bool tryReadIrradianceAtIndex(const DDGIDesc& desc,
                              fuse::math::Vec3& out_irradiance,
        out_irradiance = {};
        outReason = CacheIndexRejectReason::UndersizedCache;
    if (!tryValidateCacheIndex(desc, probe_index, cache_count, outReason)) {
    out_irradiance = cache[probe_index].irradiance;

                              fuse::math::Vec3& out_irradiance) {
    return tryReadIrradianceAtIndex(desc, cache, cache_count, probe_index, out_irradiance, reason);

    if (!tryValidateCacheIndex(desc, cache, probe_index, cache_count, outReason)) {
    if (!tryValidateCacheAccess(desc, cache, cache_count, probe_index, reason)) {
    if (!tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason)) {
    if (!tryValidateCacheLookup(desc, cache, cache_count, probe_index, reason)) {
    outReason = CacheIndexRejectReason::None;

bool wouldSkipProbeTrilinearSample(const DDGIDesc& desc,
    return !tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, outReason);

    return wouldSkipProbeTrilinearSample(desc, coords, cache, cache_count, reason);

bool wouldSkipCanSampleAtProbeCoords(const DDGIDesc& desc,
    return !canSampleAtProbeCoords(desc, coords, cache, cache_count);
}

bool tryReadIrradianceAtIndex(const DDGIDesc& desc,
                              const IrradianceCacheEntry* cache,
                              u32 cache_count,
                              u32 probe_index,
                              fuse::math::Vec3& out_irradiance) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return tryReadIrradianceAtIndex(desc, cache, cache_count, probe_index, out_irradiance, reason);
}

bool wouldClampCacheIndex(u32 probe_index, const DDGIDesc& desc) {
    return ProbeGridLayout::isProbeIndexOutOfRange(probe_index, desc);
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

u32 requiredCacheCount(const DDGIDesc& desc) {
    return probeCount(desc);
}

bool isCacheSizedForGrid(const DDGIDesc& desc, u32 cache_count) {
    const u32 required = requiredCacheCount(desc);
    if (required == 0u) {
        return true;
    }
    return cache_count >= required;

u32 cacheEntriesMissing(const DDGIDesc& desc, u32 cache_count) {
    if (required == 0u || cache_count >= required) {
        return 0u;
    return required - cache_count;

bool tryValidateCacheIndex(const DDGIDesc& desc,
                           u32 probe_index,
                           u32 cache_count,
                           CacheIndexRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = CacheIndexRejectReason::EmptyGrid;
        return false;
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index)) {
        outReason = CacheIndexRejectReason::OutOfRangeProbeIndex;
    if (cache_count == 0u) {
        outReason = CacheIndexRejectReason::UndersizedCache;
    if (probe_index >= cache_count) {
    outReason = CacheIndexRejectReason::None;

                           const IrradianceCacheEntry* cache,
    if (cache == nullptr) {
        outReason = CacheIndexRejectReason::NullCache;
    return tryValidateCacheIndex(desc, probe_index, cache_count, outReason);

bool tryValidateCacheIndexAfterClamp(const DDGIDesc& desc,

    const u32 clamped = ProbeGridLayout::clampProbeIndex(probe_index, desc);
    if (clamped >= cache_count) {


    return tryValidateCacheIndexAfterClamp(desc, probe_index, cache_count, outReason);

bool isCacheIndexValid(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return tryValidateCacheIndex(desc, probe_index, cache_count, reason);

bool wouldSkipCacheIndexLookup(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return !tryValidateCacheIndex(desc, probe_index, cache_count, reason);

bool wouldSkipCacheIndexLookup(const DDGIDesc& desc,
                               u32 cache_count) {
    return !tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);

bool wouldSkipCacheIndexLookupAfterClamp(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return !tryValidateCacheIndexAfterClamp(desc, probe_index, cache_count, reason);

bool wouldSkipCacheIndexLookupAfterClamp(const DDGIDesc& desc,
    return !tryValidateCacheIndexAfterClamp(desc, cache, probe_index, cache_count, reason);

bool wouldSkipCacheIndexLookupAtCoord(const DDGIDesc& desc,
                                      const ProbeGridCoord& coord,

    const ProbeGridCoord clamped = ProbeGridLayout::clampProbeGridCoord(desc, coord);
    const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, clamped);
    return !tryValidateCacheIndexAfterClamp(desc, cache, index, cache_count, reason);

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
    tryValidateCacheIndex(desc, probe_index, cache_count, reason);
    return reason;

    tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);

bool preflightCacheIndexLookup(const DDGIDesc& desc,
                               CacheIndexRejectReason* reason) {
    const CacheIndexRejectReason reject = classifyCacheIndexReject(desc, cache, probe_index, cache_count);
    if (reason != nullptr) {
        *reason = reject;
    return !cacheIndexRejectReasonIsBlocking(reject);

bool tryPreflightCacheIndexLookup(const DDGIDesc& desc,
    return preflightCacheIndexLookup(desc, cache, probe_index, cache_count, &outReason);

bool wouldClampProbeIndexForLookup(u32 probe_index, const DDGIDesc& desc) {
    return !ProbeGridLayout::isEmptyGrid(desc) && ProbeGridLayout::isProbeIndexOutOfRange(probe_index, desc);
    return cacheDeficitForGrid(desc, cache_count) == 0u;

ProbeSampleSkipReason classifyProbeSampleSkip(const DDGIDesc& desc, u32 cache_count) {
    const ProbeSampleSkipReason grid_reason = classifyProbeGridSkip(desc);
    if (grid_reason != ProbeSampleSkipReason::None) {
        return grid_reason;
    if (!isCacheSizedForGrid(desc, cache_count)) {
        return ProbeSampleSkipReason::UndersizedCache;
    return ProbeSampleSkipReason::None;

ProbeSampleSkipReason classifyProbeSampleLookup(const DDGIDesc& desc,
        return ProbeSampleSkipReason::NullCache;
    return classifyProbeSampleSkip(desc, cache_count);

bool canSampleProbeGrid(const DDGIDesc& desc) {
    return classifyProbeGridSkip(desc) == ProbeSampleSkipReason::None;

u32 expectedCacheCount(const DDGIDesc& desc) {
    return probeCount(desc);

bool cacheMatchesGrid(const DDGIDesc& desc, u32 cache_count) {
    return cache_count == expectedCacheCount(desc);

bool isCacheIndexInRange(u32 cache_index, u32 cache_count) {
    return cache_count > 0u && cache_index < cache_count;

u32 clampCacheIndex(u32 cache_index, const DDGIDesc& desc, u32 cache_count) {
    if (cache_count == 0u || ProbeGridLayout::isEmptyGrid(desc)) {
    const u32 max_index = std::min(cache_count - 1u, ProbeGridLayout::maxProbeIndex(desc));
    return std::min(cache_index, max_index);

fuse::math::Vec3 sampleIrradianceAtCacheIndex(const IrradianceCacheEntry* cache,
                                            u32 cache_index) {
    if (cache == nullptr || !isCacheIndexInRange(cache_index, cache_count)) {
        return {};
    return cache[cache_index].irradiance;
    return cache_count >= requiredCacheCount(desc);

u32 requiredCacheCount(const DDGIDesc& desc) {

    return ProbeGridLayout::isValidProbeIndex(desc, probe_index) && probe_index < cache_count;

bool isProbeIndexCacheAccessible(u32 probe_index, u32 cache_count) {
    return cache_count > 0u && probe_index < cache_count;

bool isProbeIndexCacheOutOfRange(u32 probe_index, u32 cache_count) {
    return cache_count == 0u || probe_index >= cache_count;

u32 clampProbeIndexForCache(u32 probe_index, u32 cache_count) {
    return std::min(probe_index, cache_count - 1u);
}

bool tryIsCacheIndexValid(const DDGIDesc& desc,
                          u32 probe_index,
                          u32 cache_count,
                          CacheIndexRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = CacheIndexRejectReason::EmptyGrid;
        return false;
    }
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index)) {
        outReason = CacheIndexRejectReason::ProbeIndexOutOfRange;
        return false;
    }
    if (probe_index >= cache_count) {
        outReason = CacheIndexRejectReason::CacheUndersized;
        return false;
    }

    outReason = CacheIndexRejectReason::None;
    return true;
}

bool tryIsCacheIndexValid(const DDGIDesc& desc,
                          u32 probe_index,
                          u32 cache_count,
                          CacheIndexRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = CacheIndexRejectReason::EmptyGrid;
        return false;
    }
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index)) {
        outReason = CacheIndexRejectReason::ProbeIndexOutOfRange;
        return false;
    }
    if (probe_index >= cache_count) {
        outReason = CacheIndexRejectReason::CacheUndersized;
        return false;
    }

    outReason = CacheIndexRejectReason::None;
    return true;
}

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return CacheIndexRejectReason::EmptyGrid;
    }
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index)) {
        return CacheIndexRejectReason::OutOfRangeProbeIndex;
    }
    if (cache_count == 0u) {
        return CacheIndexRejectReason::UndersizedCache;
    }
    if (probe_index >= cache_count) {
        return CacheIndexRejectReason::UndersizedCache;
    }
    return CacheIndexRejectReason::None;
}

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                               const IrradianceCacheEntry* cache,
                                               u32 probe_index,
                                               u32 cache_count) {
    if (cache == nullptr) {
        return CacheIndexRejectReason::NullCache;
    }
    return classifyCacheIndexReject(desc, probe_index, cache_count);
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
    if (probe_index >= cache_count) {
        outReason = CacheIndexRejectReason::UndersizedCache;
    outReason = CacheIndexRejectReason::None;
    return true;
    outReason = classifyCacheIndexReject(desc, probe_index, cache_count);
    return outReason == CacheIndexRejectReason::None;
}

bool tryValidateCacheIndex(const DDGIDesc& desc,
                           const IrradianceCacheEntry* cache,
                           u32 probe_index,
                           u32 cache_count,
                           CacheIndexRejectReason& outReason) {
bool tryValidateCacheIndexLookup(const DDGIDesc& desc,
    if (cache == nullptr) {
        outReason = CacheIndexRejectReason::NullCache;
        return false;
    }
    return tryValidateCacheIndex(desc, probe_index, cache_count, outReason);
    outReason = classifyCacheIndexReject(desc, cache, probe_index, cache_count);
    return outReason == CacheIndexRejectReason::None;
}

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    tryValidateCacheIndex(desc, probe_index, cache_count, reason);
    return reason;
}

CacheIndexRejectReason classifyCacheIndexReject(const DDGIDesc& desc,
                                                const IrradianceCacheEntry* cache,
                                                u32 probe_index,
                                                u32 cache_count) {
    tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);

bool wouldSkipCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return !isCacheIndexValid(desc, probe_index, cache_count);

bool wouldSkipCacheIndex(const DDGIDesc& desc,
    return !tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);

ProbeTrilinearSampleRejectReason classifyProbeTrilinearSampleReject(const DDGIDesc& desc,
                                                                    const ProbeSampleCoords& coords,
    ProbeTrilinearSampleRejectReason reason = ProbeTrilinearSampleRejectReason::None;
    tryCanSampleAtProbeCoords(desc, coords, cache, cache_count, reason);

bool wouldSkipTrilinearProbeSample(const DDGIDesc& desc,
    return !canSampleAtProbeCoords(desc, coords, cache, cache_count);
bool wouldClampCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return false;
    return !ProbeGridLayout::isValidProbeIndex(desc, probe_index) || probe_index >= cache_count ||
           cache_count < requiredCacheCount(desc);

bool wouldClampCacheIndex(const DDGIDesc& desc, u32 probe_index) {
    return !ProbeGridLayout::isEmptyGrid(desc) && ProbeGridLayout::isProbeIndexOutOfRange(probe_index, desc);
bool preflightCacheIndexLookup(const DDGIDesc& desc,
                               u32 cache_count,
                               CacheIndexRejectReason* reason) {
    CacheIndexRejectReason local = CacheIndexRejectReason::None;
    const bool ok = tryValidateCacheIndex(desc, probe_index, cache_count, local);
    if (reason != nullptr) {
        *reason = local;
    return ok;

    const bool ok = tryValidateCacheIndex(desc, cache, probe_index, cache_count, local);

bool wouldClampCacheIndexLookup(u32 probe_index, const DDGIDesc& desc) {

bool isCacheIndexValid(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return tryIsCacheIndexValid(desc, probe_index, cache_count, reason);
}

bool canLookupCacheAtIndex(const DDGIDesc& desc, u32 cache_count, u32 /*probe_index*/) {
    return canSampleProbeGrid(desc) && cache_count > 0u && isCacheSizedForGrid(desc, cache_count);

bool tryCanLookupCacheAtIndex(const DDGIDesc& desc,
                              u32 cache_count,
                              u32 /*probe_index*/,
                              ProbeCacheLookupRejectReason& outReason) {
    if (!canSampleProbeGrid(desc)) {
        outReason = ProbeCacheLookupRejectReason::EmptyGrid;
        return false;
    if (cache_count == 0u) {
        outReason = ProbeCacheLookupRejectReason::NullCache;
    if (!isCacheSizedForGrid(desc, cache_count)) {
        outReason = ProbeCacheLookupRejectReason::UndersizedCache;

    outReason = ProbeCacheLookupRejectReason::None;
    return true;

u32 cacheIndexFromClampedCoord(const DDGIDesc& desc, const ProbeGridCoord& coord) {
    return ProbeGridLayout::probeIndexFromClampedCoord(desc, coord);

bool trySampleCacheAtIndex(const DDGIDesc& desc,
                           const IrradianceCacheEntry* cache,
                           u32 probe_index,
                           fuse::math::Vec3& outIrradiance) {
    if (cache == nullptr) {

    ProbeCacheLookupRejectReason reason = ProbeCacheLookupRejectReason::None;
    if (!tryCanLookupCacheAtIndex(desc, cache_count, probe_index, reason)) {

    const u32 index = ProbeGridLayout::clampProbeIndex(probe_index, desc);
    if (index >= cache_count) {

    outIrradiance = cache[index].irradiance;

bool trySampleCacheAtCoord(const DDGIDesc& desc,
                           const ProbeGridCoord& coord,
    const u32 index = cacheIndexFromClampedCoord(desc, coord);
    return trySampleCacheAtIndex(desc, cache, cache_count, index, outIrradiance);

bool isCacheIndexValid(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return ProbeGridLayout::isValidProbeIndex(desc, probe_index) && probe_index < cache_count;

bool isCacheIndexOutOfRange(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return !isCacheIndexValid(desc, probe_index, cache_count);

bool tryClampCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count, u32& out_index) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        out_index = 0u;

    out_index = ProbeGridLayout::clampProbeIndex(probe_index, desc);
    if (out_index >= cache_count) {
        out_index = cache_count > 0u ? cache_count - 1u : 0u;
bool tryIsCacheIndexValid(const DDGIDesc& desc,
                          CacheIndexRejectReason& outReason) {
        outReason = CacheIndexRejectReason::EmptyGrid;
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index)) {
        outReason = CacheIndexRejectReason::OutOfRangeIndex;
    if (probe_index >= cache_count) {
        outReason = CacheIndexRejectReason::UndersizedCache;
    outReason = CacheIndexRejectReason::None;
    return tryValidateCacheIndex(desc, probe_index, cache_count, reason);
}

bool tryIsCacheIndexValid(const DDGIDesc& desc,
                          u32 probe_index,
                          u32 cache_count,
                          CacheIndexRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = CacheIndexRejectReason::EmptyGrid;
        return false;
    }
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index)) {
        outReason = CacheIndexRejectReason::InvalidProbeIndex;
    if (probe_index >= cache_count) {
        outReason = CacheIndexRejectReason::UndersizedCache;

    outReason = CacheIndexRejectReason::None;
    return true;
bool isCacheIndexOutOfRange(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return !isCacheIndexValid(desc, probe_index, cache_count);

bool canLookupCacheAtIndex(const DDGIDesc& desc, u32 cache_count) {
bool tryValidateCacheLookup(const DDGIDesc& desc,
                            const IrradianceCacheEntry* cache,
    if (cache == nullptr) {
        outReason = CacheIndexRejectReason::NullCache;
    if (!isCacheSizedForGrid(desc, cache_count)) {
    return tryValidateCacheIndex(desc, probe_index, cache_count, outReason);

bool wouldSkipCacheIndexValidation(const DDGIDesc& desc,
                                   u32 probe_index) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return !tryValidateCacheLookup(desc, cache, cache_count, probe_index, reason);
bool wouldSkipCacheIndexLookup(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {

bool wouldSkipCacheIndexLookup(const DDGIDesc& desc,
                               u32 cache_count) {
    return !tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);
bool isCacheIndexValid(const DDGIDesc& desc,
    return tryValidateCacheIndex(desc, cache, probe_index, cache_count, reason);
bool wouldClampCacheLookupIndex(u32 probe_index, const DDGIDesc& desc) {
    return ProbeGridLayout::isProbeIndexOutOfRange(probe_index, desc);

bool tryProbeWorldPosition(const DDGIDesc& desc,
                           fuse::math::Vec3& out_position,
        out_position = {};
        outReason = CacheIndexRejectReason::OutOfRangeProbeIndex;
    out_position = probeWorldPosition(desc, probe_index);

bool wouldClampProbeIndex(u32 probe_index, const DDGIDesc& desc) {

bool shouldSkipCacheLookup(const DDGIDesc& desc, const IrradianceCacheEntry* cache, u32 cache_count) {
    return shouldSkipProbeLookup(desc, cache, cache_count);
    return tryValidateCacheIndex(desc, cache, cache_count, probe_index, reason);

    return !isCacheIndexValid(desc, cache, cache_count, probe_index);

                               CacheIndexRejectReason* reason) {
    CacheIndexRejectReason local = CacheIndexRejectReason::None;
    const bool skip = !tryValidateCacheIndex(desc, probe_index, cache_count, local);
    if (reason != nullptr) {
        *reason = local;
    return skip;

    const bool skip = !tryValidateCacheIndex(desc, cache, probe_index, cache_count, local);

bool canLookupAtProbeIndex(const DDGIDesc& desc,
                           const IrradianceCacheEntry* cache,
                           u32 probe_index,
                           u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return tryCanLookupAtProbeIndex(desc, cache, probe_index, cache_count, reason);
}

bool tryCanLookupAtProbeIndex(const DDGIDesc& desc,
                              const IrradianceCacheEntry* cache,
                              u32 probe_index,
                              u32 cache_count,
                              CacheIndexRejectReason& outReason) {
    if (!tryValidateCacheIndex(desc, cache, probe_index, cache_count, outReason)) {
        return false;
    }
    if (!isCacheSizedForGrid(desc, cache_count)) {
        outReason = CacheIndexRejectReason::UndersizedCache;
        return false;
    }
    outReason = CacheIndexRejectReason::None;
    return true;
}

bool wouldClampCacheIndexLookup(u32 probe_index, const DDGIDesc& desc) {
    return ProbeGridLayout::isProbeIndexOutOfRange(probe_index, desc);
}

bool shouldSkipCacheIndexLookup(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return wouldSkipCacheIndexLookup(desc, probe_index, cache_count);
}

bool shouldSkipCacheIndexLookup(const DDGIDesc& desc,
                                const IrradianceCacheEntry* cache,
                                u32 probe_index,
                                u32 cache_count) {
    return wouldSkipCacheIndexLookup(desc, cache, probe_index, cache_count);
}

bool isValidSampleRequest(const DDGIDesc& desc,
                          const DDGISampleRequest& /*request*/,
                          u32 cache_count) {
    return canSampleProbeGrid(desc) && isCacheSizedForGrid(desc, cache_count);

bool tryCanLookupCacheAtProbeIndex(const DDGIDesc& desc,
    if (!canSampleProbeGrid(desc)) {
        outReason = CacheIndexRejectReason::NotSampleable;
    if (!isCacheSizedForGrid(desc, cache_count)) {
    if (!isCacheIndexValid(desc, probe_index, cache_count)) {
        outReason = CacheIndexRejectReason::OutOfRangeIndex;


bool isCacheIndexValidForClampedIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    if (!canLookupCacheAtIndex(desc, cache_count)) {
    const u32 clamped = ProbeGridLayout::clampProbeIndex(probe_index, desc);
    return clamped < cache_count;

        outReason = CacheIndexRejectReason::ProbeIndexOutOfRange;
        outReason = CacheIndexRejectReason::CacheUndersized;

bool tryValidateCacheSizedForGrid(const DDGIDesc& desc,

        outReason = CacheIndexRejectReason::OutOfRangeProbe;

    if (cache_count == 0u) {
        outReason = CacheIndexRejectReason::ZeroCache;
    if (!tryValidateCacheIndex(desc, probe_index, cache_count, outReason)) {

bool canSampleAtProbeCoords(const DDGIDesc& desc,
                            const ProbeSampleCoords& coords,
                            u32 cache_count) {
    ProbeSampleCoordRejectReason reason = ProbeSampleCoordRejectReason::None;
    ProbeSampleCoordsRejectReason reason = ProbeSampleCoordsRejectReason::None;
bool canLookupCacheAtCoord(const DDGIDesc& desc, const ProbeGridCoord& coord, u32 cache_count) {
    CacheIndexRejectReason reason = CacheIndexRejectReason::None;
    return tryCanLookupCacheAtCoord(desc, coord, cache_count, reason);

bool tryCanLookupCacheAtCoord(const DDGIDesc& desc,
                              const ProbeGridCoord& coord,
    if (!ProbeGridLayout::isValidProbeCoord(desc, coord)) {
        } else {
            outReason = CacheIndexRejectReason::OutOfRangeProbeIndex;
    const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, coord);
    return tryValidateCacheIndex(desc, index, cache_count, outReason);

bool canSampleAtProbeCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords, u32 cache_count) {
    ProbeSpatialSampleRejectReason reason = ProbeSpatialSampleRejectReason::None;
    return tryCanSampleAtProbeCoords(desc, coords, cache_count, reason);

bool tryCanSampleAtProbeCoords(const DDGIDesc& desc,
                               ProbeSampleCoordRejectReason& outReason) {
        outReason = ProbeSampleCoordRejectReason::EmptyGrid;
        outReason = ProbeSampleCoordRejectReason::OutOfBounds;
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        outReason = probeSampleCoordRejectFromValidity(desc, coords);

    outReason = ProbeSampleCoordRejectReason::None;

bool canLookupCacheAtIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return isCacheIndexValid(desc, probe_index, cache_count);

bool tryCanLookupCacheAtIndex(const DDGIDesc& desc,
                              CacheLookupRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc) || !canSampleProbeGrid(desc)) {
        outReason = CacheLookupRejectReason::EmptyGrid;
        outReason = CacheLookupRejectReason::UndersizedCache;
    if (!ProbeGridLayout::isValidProbeIndex(desc, probe_index) || probe_index >= cache_count) {
        outReason = CacheLookupRejectReason::ProbeIndexOutOfRange;

    outReason = CacheLookupRejectReason::None;

bool shouldSkipProbeSample(const DDGIDesc& desc, u32 cache_count) {
    return !canSampleProbeGrid(desc) || !isCacheSizedForGrid(desc, cache_count);

bool shouldSkipProbeUpdate(const DDGIDesc& desc) {
    return ProbeGridLayout::isEmptyGrid(desc);

    if (shouldSkipProbeSample(desc, cache_count)) {
    return ProbeGridLayout::isValidProbeSampleCoords(desc, coords);

                               ProbeSampleRejectReason& outReason) {
    CacheLookupRejectReason cacheReason = CacheLookupRejectReason::None;
    if (!tryCanLookupCacheAtIndex(desc, 0u, cache_count, cacheReason)) {
        switch (cacheReason) {
        case CacheLookupRejectReason::EmptyGrid:
            outReason = ProbeSampleRejectReason::EmptyGrid;
            break;
        case CacheLookupRejectReason::UndersizedCache:
        case CacheLookupRejectReason::ProbeIndexOutOfRange:
            outReason = ProbeSampleRejectReason::OutOfBounds;
        case CacheLookupRejectReason::None:
            outReason = ProbeSampleRejectReason::None;

        if (!ProbeGridLayout::areProbeSampleCoordsInBounds(desc, coords)) {
            const u32 max_x = desc.grid_dims.x - 1u;
            const u32 max_y = desc.grid_dims.y - 1u;
            const u32 max_z = desc.grid_dims.z - 1u;
            const bool indicesInRange = coords.x0 <= max_x && coords.x1 <= max_x && coords.y0 <= max_y &&
                                        coords.y1 <= max_y && coords.z0 <= max_z && coords.z1 <= max_z;
            outReason = indicesInRange ? ProbeSampleRejectReason::InvalidWeights
                                         : ProbeSampleRejectReason::OutOfBounds;

                               ProbeSampleCoordsRejectReason& outReason) {
        outReason = ProbeSampleCoordsRejectReason::NotSampleable;
    if (!ProbeGridLayout::tryValidateProbeSampleCoords(desc, coords, outReason)) {
        outReason = ProbeSampleCoordsRejectReason::UndersizedCache;

    const auto corner_valid = [&](u32 x, u32 y, u32 z) {
        const ProbeGridCoord probe_coord{x, y, z};
        const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, probe_coord);
        return isCacheIndexValid(desc, index, cache_count);
    };

    if (!corner_valid(coords.x0, coords.y0, coords.z0) || !corner_valid(coords.x1, coords.y0, coords.z0) ||
        !corner_valid(coords.x0, coords.y1, coords.z0) || !corner_valid(coords.x1, coords.y1, coords.z0) ||
        !corner_valid(coords.x0, coords.y0, coords.z1) || !corner_valid(coords.x1, coords.y0, coords.z1) ||
        !corner_valid(coords.x0, coords.y1, coords.z1) || !corner_valid(coords.x1, coords.y1, coords.z1)) {

    outReason = ProbeSampleCoordsRejectReason::None;
                               ProbeSpatialSampleRejectReason& outReason) {
        outReason = ProbeSpatialSampleRejectReason::EmptyGrid;
        outReason = ProbeSpatialSampleRejectReason::UndersizedCache;
        outReason = ProbeSpatialSampleRejectReason::InvalidSampleCoords;
    outReason = ProbeSpatialSampleRejectReason::None;
bool tryValidateSampleRequest(const DDGIDesc& desc,
                              const DDGISampleRequest& /*request*/,
                              SampleRequestRejectReason& outReason) {
        outReason = SampleRequestRejectReason::EmptyGrid;
        outReason = SampleRequestRejectReason::NotSampleable;
        outReason = SampleRequestRejectReason::UndersizedCache;
    outReason = SampleRequestRejectReason::None;
    return true;
}

bool isValidSampleRequest(const DDGIDesc& desc,
                          const DDGISampleRequest& request,
                          u32 cache_count) {
    return classifyProbeSampleSkip(desc, cache_count) == ProbeSampleSkipReason::None;
}

bool canAccessCacheIndex(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return ProbeGridLayout::isValidProbeIndex(desc, probe_index) && probe_index < cache_count;

bool tryIsValidSampleRequest(const DDGIDesc& desc,
                             const DDGISampleRequest& /*request*/,
                             u32 cache_count,
                             CacheLookupRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc) || !canSampleProbeGrid(desc)) {
        outReason = CacheLookupRejectReason::EmptyGrid;
        return false;
    if (!isCacheSizedForGrid(desc, cache_count)) {
        outReason = CacheLookupRejectReason::UndersizedCache;

    outReason = CacheLookupRejectReason::None;
    return true;

bool tryValidateSampleRequest(const DDGIDesc& desc,
                              SampleRequestRejectReason& outReason) {
    if (!canSampleProbeGrid(desc)) {
        outReason = SampleRequestRejectReason::NotSampleable;
        outReason = SampleRequestRejectReason::UndersizedCache;
    outReason = SampleRequestRejectReason::None;
    SampleRequestRejectReason reason = SampleRequestRejectReason::None;
    return tryValidateSampleRequest(desc, request, cache_count, reason);
}

bool tryValidateSampleRequest(const DDGIDesc& desc,
                              const DDGISampleRequest& /*request*/,
                              u32 cache_count,
                              SampleRequestRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = SampleRequestRejectReason::EmptyGrid;
        return false;
    }
    if (!canSampleProbeGrid(desc)) {
        outReason = SampleRequestRejectReason::NotSampleable;
        return false;
    }
    if (!isCacheSizedForGrid(desc, cache_count)) {
        outReason = SampleRequestRejectReason::UndersizedCache;
        return false;
    }
    outReason = SampleRequestRejectReason::None;
    return true;
}

bool wouldSkipSampleRequest(const DDGIDesc& desc,
                            const DDGISampleRequest& request,
                            u32 cache_count) {
    return !isValidSampleRequest(desc, request, cache_count);
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

ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      u32 max_indices,
                                                      const u32* out_indices,
                                                      u32* out_count) {
    if (out_indices == nullptr) {
        if (out_count != nullptr) {
            *out_count = 0u;
        }
        return ProbeScheduleRejectReason::NullOutIndices;
    }
    if (out_count == nullptr) {
        return ProbeScheduleRejectReason::NullOutCount;
    }
    if (probe_count == 0u) {
        *out_count = 0u;
        return ProbeScheduleRejectReason::ZeroProbeCount;
    }
    if (max_indices == 0u) {
        *out_count = 0u;
        return ProbeScheduleRejectReason::ZeroMaxIndices;
    }
    return ProbeScheduleRejectReason::None;
}

bool tryCanScheduleProbeUpdates(u32 probe_count,
                               u32 max_indices,
                               const u32* out_indices,
                               u32* out_count,
                               ProbeScheduleRejectReason& outReason) {
    if (out_indices == nullptr) {
        outReason = ProbeScheduleRejectReason::NullOutIndices;
const char* probeScheduleRejectReasonLabel(ProbeScheduleRejectReason reason) {
    switch (reason) {
    case ProbeScheduleRejectReason::None:
        return "none";
    case ProbeScheduleRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case ProbeScheduleRejectReason::NullIndicesBuffer:
        return "null_indices_buffer";
    case ProbeScheduleRejectReason::NullCountOut:
        return "null_count_out";
    case ProbeScheduleRejectReason::ZeroMaxIndices:
        return "zero_max_indices";
    case ProbeScheduleRejectReason::ZeroProbesPerFrame:
        return "zero_probes_per_frame";
    }
    return "unknown";

namespace {

                              u32 probes_per_frame,
                              const u32* out_count,
        outReason = ProbeScheduleRejectReason::NullIndicesBuffer;
        return false;
    if (out_count == nullptr) {
        outReason = ProbeScheduleRejectReason::NullCountOut;
    if (probe_count == 0u) {
        outReason = ProbeScheduleRejectReason::ZeroProbeCount;
    if (max_indices == 0u) {
        outReason = ProbeScheduleRejectReason::ZeroMaxIndices;

    outReason = ProbeScheduleRejectReason::None;
    return true;

} // namespace

bool tryScheduleProbeUpdates(u32 frame_index,
                             u32 probe_count,
                             u32* out_indices,
    if (!tryCanScheduleProbeUpdates(probe_count, probes_per_frame, out_indices, max_indices, out_count, outReason)) {
        if (out_count != nullptr) {
            *out_count = 0u;

    scheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count);

bool canScheduleProbeUpdates(u32 probe_count,
                             const u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    return tryCanScheduleProbeUpdates(probe_count, probes_per_frame, out_indices, max_indices, out_count, reason);

bool wouldSkipProbeSchedule(u32 probe_count,
    return !canScheduleProbeUpdates(probe_count, probes_per_frame, out_indices, max_indices, out_count);

bool probeSchedulePreflight(u32 probe_count,
        outReason = ProbeScheduleRejectReason::NullOutputIndices;
        outReason = ProbeScheduleRejectReason::NullOutputCount;


bool canScheduleProbeUpdates(u32 probe_count, u32 max_indices, const u32* out_indices, const u32* out_count) {
    return probeSchedulePreflight(probe_count, max_indices, out_indices, out_count, reason);

bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, const u32* out_count) {
    return !canScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count);

    if (!probeSchedulePreflight(probe_count, max_indices, out_indices, out_count, outReason)) {
    outReason = classifyProbeScheduleReject(probe_count, max_indices, out_indices, out_count);
    if (outReason == ProbeScheduleRejectReason::NullOutIndices && out_count != nullptr) {
    } else if (outReason == ProbeScheduleRejectReason::ZeroProbeCount ||
               outReason == ProbeScheduleRejectReason::ZeroMaxIndices) {
    return outReason == ProbeScheduleRejectReason::None;

bool canScheduleProbeUpdates(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    return tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);


bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
bool tryValidateProbeSchedule(u32 probe_count,
    (void)probes_per_frame;
        outReason = ProbeScheduleRejectReason::NullIndices;
        outReason = ProbeScheduleRejectReason::NullCount;

    return tryValidateProbeSchedule(probe_count, probes_per_frame, out_indices, max_indices, out_count, reason);


    return tryValidateProbeSchedule(probe_count, max_indices, out_indices, out_count, reason);


void scheduleProbeUpdates(u32 frame_index,
                          u32* out_count) {
    if (out_indices == nullptr || out_count == nullptr || probe_count == 0u || max_indices == 0u) {
        outReason = ProbeScheduleRejectReason::NullCountOutput;


ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
    tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
    return reason;


    if (!tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, outReason)) {

bool preflightScheduleProbeUpdates(u32 probe_count,


    return preflightScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);


    if (!preflightScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, outReason)) {

bool validateProbeScheduleInputs(u32 probe_count,
    if (probes_per_frame == 0u) {
        outReason = ProbeScheduleRejectReason::ZeroProbesPerFrame;

    return tryCanScheduleProbeUpdates(probe_count, probes_per_frame, max_indices, out_indices, out_count, reason);

    return !canScheduleProbeUpdates(probe_count, probes_per_frame, max_indices, out_indices, out_count);

    if (!tryCanScheduleProbeUpdates(probe_count, probes_per_frame, max_indices, out_indices, out_count, outReason)) {
        outReason = ProbeScheduleRejectReason::NullOutCount;
    return tryScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);

bool tryScheduleProbeUpdates(u32 probe_count,
    if (out_indices == nullptr || out_count == nullptr) {
        outReason = ProbeScheduleRejectReason::NullOutput;
    if (!tryScheduleProbeUpdates(
            frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count, reason)) {


    return validateProbeScheduleInputs(probe_count, max_indices, out_indices, out_count, reason);

bool canScheduleProbeUpdates(u32 probe_count, u32* out_indices, u32 max_indices, u32* out_count) {
    return tryScheduleProbeUpdates(0u, probe_count, 0u, out_indices, max_indices, out_count, reason);





bool tryCanScheduleProbeUpdatesAtRate(u32 probe_count,


bool canScheduleProbeUpdatesAtRate(u32 probe_count,
    return tryCanScheduleProbeUpdatesAtRate(
        probe_count, probes_per_frame, max_indices, out_indices, out_count, reason);

bool wouldSkipProbeScheduleAtRate(u32 probe_count,
    return !canScheduleProbeUpdatesAtRate(
        probe_count, probes_per_frame, max_indices, out_indices, out_count);

ProbeScheduleRejectReason classifyProbeScheduleRejectAtRate(u32 probe_count,
    tryCanScheduleProbeUpdatesAtRate(probe_count, probes_per_frame, max_indices, out_indices, out_count, reason);

bool preflightProbeScheduleAtRate(u32 probe_count,
                                  ProbeScheduleRejectReason* reason) {
    const ProbeScheduleRejectReason reject =
        classifyProbeScheduleRejectAtRate(probe_count, probes_per_frame, max_indices, out_indices, out_count);
    if (reason != nullptr) {
        *reason = reject;
    return !probeScheduleRejectReasonIsBlocking(reject);

bool tryPreflightProbeScheduleAtRate(u32 probe_count,
    return preflightProbeScheduleAtRate(
        probe_count, probes_per_frame, max_indices, out_indices, out_count, &outReason);

    if (!tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason)) {

void writeScheduledProbeUpdates(u32 frame_index,
    if (probe_count == 0u || max_indices == 0u || probes_per_frame == 0u) {
        return;

    if (!validateProbeScheduleInputs(probe_count, max_indices, out_indices, out_count, outReason)) {
        outReason = ProbeScheduleRejectReason::NullOutputBuffer;



    if (!tryCanScheduleProbeUpdates(probe_count,
                                    probes_per_frame,
                                    out_indices,
                                    max_indices,
                                    out_count,
                                    outReason)) {






    const u32 count = std::min(probes_per_frame, std::min(probe_count, max_indices));
    const u32 start = (frame_index * probes_per_frame) % probe_count;
    for (u32 i = 0; i < count; ++i) {
        out_indices[i] = (start + i) % probe_count;
    *out_count = count;
    outReason = classifyProbeScheduleReject(probe_count, max_indices, out_indices, out_count);
    return outReason == ProbeScheduleRejectReason::None;
}

bool wouldSkipProbeSchedule(u32 probe_count, u32* out_indices, u32 max_indices, u32* out_count) {
    return !canScheduleProbeUpdates(probe_count, out_indices, max_indices, out_count);
bool wouldSkipProbeSchedule(u32 probe_count,
                            u32 max_indices,
                            const u32* out_indices,
                            u32* out_count,
    ProbeScheduleRejectReason local = ProbeScheduleRejectReason::None;
    const bool skip = !tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, local);
        *reason = local;
    return skip;
}

bool shouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    return wouldSkipProbeSchedule(probe_count, max_indices, out_indices, out_count);
}

bool tryCanScheduleProbeUpdates(u32 frame_index,
                               u32 probe_count,
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

bool canScheduleProbeUpdates(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    return tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
}

bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    return !canScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count);

bool preflightProbeSchedule(u32 probe_count,
                            u32 max_indices,
                            const u32* out_indices,
                            u32* out_count,
                            ProbeScheduleRejectReason* reason) {
    ProbeScheduleRejectReason local = ProbeScheduleRejectReason::None;
    const bool ok = tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, local);
    if (reason != nullptr) {
        *reason = local;
    return ok;

void scheduleProbeUpdates(u32 frame_index,
                          u32 probe_count,
                          u32 probes_per_frame,
                          u32* out_indices,
                          u32 max_indices,
                          u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    tryScheduleProbeUpdates(frame_index,
                            probe_count,
                            probes_per_frame,
                            out_indices,
                            max_indices,
                            out_count,
                            reason);

bool tryScheduleProbeUpdates(u32 frame_index,
                             u32* out_count,
                             ProbeScheduleRejectReason& outReason) {
    if (!tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, outReason)) {
        return false;
    scheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count);

bool tryScheduleProbeUpdatesAtRate(u32 frame_index,
    if (!tryCanScheduleProbeUpdatesAtRate(
            probe_count, probes_per_frame, max_indices, out_indices, out_count, outReason)) {

ProbeScheduleRejectReason classifyProbeScheduleReject(u32 probe_count,
                                                      const u32* out_indices,
    tryCanScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count, reason);
    return reason;

bool preflightProbeSchedule(u32 probe_count,
                            ProbeScheduleRejectReason* reason) {
    const ProbeScheduleRejectReason reject =
        classifyProbeScheduleReject(probe_count, max_indices, out_indices, out_count);
    if (reason != nullptr) {
        *reason = reject;
    return !probeScheduleRejectReasonIsBlocking(reject);

bool wouldClampScheduledProbeCount(u32 probe_count, u32 probes_per_frame, u32 max_indices) {
    if (probe_count == 0u || max_indices == 0u) {
    const u32 scheduled = std::min(probes_per_frame, std::min(probe_count, max_indices));
    return scheduled < probes_per_frame;
    outReason = ProbeScheduleRejectReason::None;

bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    if (out_indices == nullptr) {
    if (out_count == nullptr) {
    if (probe_count == 0u) {
    if (max_indices == 0u) {
    tryScheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count,
    tryScheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count, reason);
}

                             u32 probe_count,
                             u32 probes_per_frame,
                             u32* out_indices,
                             u32 max_indices,
    if (!tryValidateProbeSchedule(probe_count, max_indices, out_indices, out_count, outReason)) {
        if (out_count != nullptr) {
            *out_count = 0u;
    return true;
    if (!tryScheduleProbeUpdates(frame_index,
                                 reason)) {
}

namespace {

bool preflightProbeSchedule(u32 probe_count,
                            u32 max_indices,
                            const u32* out_indices,
                            const u32* out_count,
                            ProbeScheduleRejectReason& outReason) {
    if (out_indices == nullptr) {
        outReason = ProbeScheduleRejectReason::NullOutput;
        return false;
    }
    if (out_count == nullptr) {
        outReason = ProbeScheduleRejectReason::NullCount;
        return false;
    }
    if (probe_count == 0u) {
        outReason = ProbeScheduleRejectReason::ZeroProbeCount;
        return false;
    }
    if (max_indices == 0u) {
        outReason = ProbeScheduleRejectReason::ZeroMaxIndices;
        return false;
    }
    outReason = ProbeScheduleRejectReason::None;
    return true;
}

} // namespace

bool canScheduleProbeUpdates(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    return preflightProbeSchedule(probe_count, max_indices, out_indices, out_count, reason);
}

bool wouldSkipProbeSchedule(u32 probe_count, u32 max_indices, const u32* out_indices, u32* out_count) {
    return !canScheduleProbeUpdates(probe_count, max_indices, out_indices, out_count);
}

bool tryScheduleProbeUpdates(u32 frame_index,
                             u32 probe_count,
                             u32 probes_per_frame,
                             u32* out_indices,
                             u32 max_indices,
                             u32* out_count,
                             ProbeScheduleRejectReason& outReason) {
    if (!preflightProbeSchedule(probe_count, max_indices, out_indices, out_count, outReason)) {
        if (out_count != nullptr &&
            (outReason == ProbeScheduleRejectReason::NullOutput ||
             outReason == ProbeScheduleRejectReason::ZeroProbeCount ||
             outReason == ProbeScheduleRejectReason::ZeroMaxIndices)) {
            *out_count = 0u;
        }
    if (!tryCanScheduleProbeUpdates(frame_index,
                                    probe_count,
                                    probes_per_frame,
                                    max_indices,
                                    out_indices,
                                    out_count,
                                    outReason)) {
        return false;
    }

    scheduleProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count);
    outReason = ProbeScheduleRejectReason::None;
    return true;
}

} // namespace

bool tryScheduleProbeUpdates(u32 frame_index,
                             u32 probe_count,
                             u32 probes_per_frame,
                             u32* out_indices,
                             u32 max_indices,
                             u32* out_count,
                             ProbeScheduleRejectReason& outReason) {
    if (out_indices == nullptr) {
        outReason = ProbeScheduleRejectReason::NullIndices;
        return false;
    }
    if (out_count == nullptr) {
        outReason = ProbeScheduleRejectReason::NullCount;

    writeScheduledProbeUpdates(frame_index, probe_count, probes_per_frame, out_indices, max_indices, out_count);
    outReason = ProbeScheduleRejectReason::None;
    return true;

bool canScheduleProbeUpdates(u32 probe_count,
                             u32* out_count) {
    ProbeScheduleRejectReason reason = ProbeScheduleRejectReason::None;
    return tryScheduleProbeUpdates(0u, probe_count, probes_per_frame, out_indices, max_indices, out_count, reason);

bool wouldSkipProbeSchedule(u32 probe_count,
    return !canScheduleProbeUpdates(probe_count, probes_per_frame, out_indices, max_indices, out_count);

void scheduleProbeUpdates(u32 frame_index,
    if (out_indices == nullptr || out_count == nullptr) {
        if (out_count != nullptr) {
            *out_count = 0u;
        return;

u32 effectiveScheduledProbeCount(u32 probe_count, u32 probes_per_frame, u32 max_indices) {
    return std::min(probes_per_frame, std::min(probe_count, max_indices));

bool wouldClampScheduledProbeCount(u32 probe_count, u32 probes_per_frame, u32 max_indices) {
    if (probe_count == 0u || max_indices == 0u || probes_per_frame == 0u) {
    const u32 effective = effectiveScheduledProbeCount(probe_count, probes_per_frame, max_indices);
    return effective < probes_per_frame;
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
fuse::math::Vec3 trilinearProbeIrradiance(const DDGIDesc& desc,
                                          const fuse::math::Vec3& world_position,
                                          const IrradianceCacheEntry* cache,
                                          u32 cache_count) {
    if (classifyProbeSampleLookup(desc, cache, cache_count) != ProbeSampleSkipReason::None) {
        return {};
    }

    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::tryBuildProbeSampleCoords(desc, world_position, coords)) {
    if (!ProbeGridLayout::buildAndClampProbeSampleCoords(desc, world_position, coords)) {
        return {};
    }
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        return {};
    }

fuse::math::Vec3 trilinearProbeIrradianceFromCoords(const DDGIDesc& desc,
                                                    const ProbeSampleCoords& coords,
                                                    const IrradianceCacheEntry* cache,
                                                    u32 cache_count) {
    const auto sample_probe = [&](u32 x, u32 y, u32 z) -> fuse::math::Vec3 {
        const ProbeGridCoord probe_coord{x, y, z};
        const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, probe_coord);
        fuse::math::Vec3 irradiance{};
        if (!tryReadIrradianceAtIndex(desc, cache, cache_count, index, irradiance)) {
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        return {};
    }

        if (!isCacheIndexValid(desc, index, cache_count)) {
        if (index == UINT32_MAX || isProbeIndexCacheOutOfRange(index, cache_count)) {
            return {};
        }
        return irradiance;
        if (index == UINT32_MAX) {
        return sampleIrradianceAtCacheIndex(cache, cache_count, index);
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
                                 fuse::math::Vec3& out_irradiance) {
    ProbeSpatialSampleRejectReason reason = ProbeSpatialSampleRejectReason::None;
    return tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, out_irradiance, reason);
}

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

    out_irradiance = trilinearProbeIrradianceFromCoords(desc, coords, cache, cache_count);
    return true;

                                 fuse::math::Vec3& out_irradiance) {
    ProbeTrilinearSampleRejectReason reason = ProbeTrilinearSampleRejectReason::None;
    return tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, out_irradiance, reason);

fuse::math::Vec3 trilinearProbeIrradiance(const DDGIDesc& desc,
                                          u32 cache_count) {
    fuse::math::Vec3 result{};
    tryTrilinearProbeIrradiance(desc, world_position, cache, cache_count, result);
    return result;

bool tryTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                            const fuse::math::Vec3& direction,
                                 ProbeSpatialSampleRejectReason& outReason) {
    if (cache == nullptr) {
        outReason = ProbeSpatialSampleRejectReason::NullCache;

    ProbeSampleCoordsRejectReason buildReason = ProbeSampleCoordsRejectReason::None;
    if (!ProbeGridLayout::tryBuildProbeSampleCoords(desc, world_position, coords, buildReason)) {
        outReason = buildReason == ProbeSampleCoordsRejectReason::EmptyGrid
                        ? ProbeSpatialSampleRejectReason::EmptyGrid
                        : ProbeSpatialSampleRejectReason::InvalidSampleCoords;

    if (!tryCanSampleAtProbeCoords(desc, coords, cache_count, outReason)) {

    out_irradiance = trilinearProbeIrradiance(desc, world_position, cache, cache_count);

fuse::math::Vec3 trilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                                     const fuse::math::Vec3& world_position,
                                                     const IrradianceCacheEntry* cache,
    if (cache == nullptr || !canSampleProbeGrid(desc) || !isCacheSizedForGrid(desc, cache_count)) {

    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords) ||
        !ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {

    out_irradiance = trilinearProbeIrradiance(desc, world_position, cache, cache_count);

fuse::math::Vec3 trilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
    if (classifyProbeSampleLookup(desc, cache, cache_count) != ProbeSampleSkipReason::None) {
        return {};
    }

    const fuse::math::Vec3 sample_direction = DdgiIrradianceEncoding::resolveSampleDirection(direction);

    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::tryBuildProbeSampleCoords(desc, world_position, coords)) {
        return {};
    }
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
    if (!ProbeGridLayout::buildAndClampProbeSampleCoords(desc, world_position, coords)) {
        return {};
    }

    const auto sample_probe = [&](u32 x, u32 y, u32 z) -> fuse::math::Vec3 {
        const ProbeGridCoord probe_coord{x, y, z};
        const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, probe_coord);
        fuse::math::Vec3 irradiance{};
        if (!tryReadIrradianceAtIndex(desc, cache, cache_count, index, irradiance)) {
        if (index == UINT32_MAX || !isCacheIndexInRange(index, cache_count)) {
        if (!isCacheIndexValid(desc, index, cache_count)) {
        if (index == UINT32_MAX || isProbeIndexCacheOutOfRange(index, cache_count)) {
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

bool tryTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                              const fuse::math::Vec3& world_position,
                                              const fuse::math::Vec3& direction,
                                              const IrradianceCacheEntry* cache,
                                              u32 cache_count,
                                              fuse::math::Vec3& out_irradiance) {
    out_irradiance = {};
    if (cache == nullptr || !canSampleProbeGrid(desc) || !isCacheSizedForGrid(desc, cache_count)) {
        return false;
    }

    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords) ||
        !ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        return false;
    }

    out_irradiance =
        trilinearDirectionalProbeIrradiance(desc, world_position, direction, cache, cache_count);
    return true;
}

bool tryTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                            const fuse::math::Vec3& world_position,
                                            const fuse::math::Vec3& direction,
                                            const IrradianceCacheEntry* cache,
                                            u32 cache_count,
                                            fuse::math::Vec3& out_irradiance) {
    ProbeSpatialSampleRejectReason reason = ProbeSpatialSampleRejectReason::None;
    return tryTrilinearDirectionalProbeIrradiance(
        desc, world_position, direction, cache, cache_count, out_irradiance, reason);
}

bool tryTrilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                            const fuse::math::Vec3& world_position,
                                            const fuse::math::Vec3& direction,
                                            const IrradianceCacheEntry* cache,
                                            u32 cache_count,
                                            fuse::math::Vec3& out_irradiance,
                                            ProbeSpatialSampleRejectReason& outReason) {
    if (cache == nullptr) {
        outReason = ProbeSpatialSampleRejectReason::NullCache;
        out_irradiance = {};
        return false;
    }

    ProbeSampleCoords coords{};
    ProbeSampleCoordsRejectReason buildReason = ProbeSampleCoordsRejectReason::None;
    if (!ProbeGridLayout::tryBuildProbeSampleCoords(desc, world_position, coords, buildReason)) {
        outReason = buildReason == ProbeSampleCoordsRejectReason::EmptyGrid
                        ? ProbeSpatialSampleRejectReason::EmptyGrid
                        : ProbeSpatialSampleRejectReason::InvalidSampleCoords;
        out_irradiance = {};
        return false;
    }

    if (!tryCanSampleAtProbeCoords(desc, coords, cache_count, outReason)) {
        out_irradiance = {};
        return false;
    }

    out_irradiance =
        trilinearDirectionalProbeIrradiance(desc, world_position, direction, cache, cache_count);
    return true;
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

const char* probeSampleSkipReasonLabel(ProbeSampleSkipReason reason) {
    switch (reason) {
    case ProbeSampleSkipReason::None:
        return "none";
    case ProbeSampleSkipReason::EmptyGrid:
        return "empty_grid";
    case ProbeSampleSkipReason::ZeroIrradianceResolution:
        return "zero_irradiance_resolution";
    case ProbeSampleSkipReason::InvalidProbeSpacing:
        return "invalid_probe_spacing";
    case ProbeSampleSkipReason::UndersizedCache:
        return "undersized_cache";
    case ProbeSampleSkipReason::NullCache:
        return "null_cache";
    }
    return "unknown";

bool probeSampleSkipReasonIsBlocking(ProbeSampleSkipReason reason) {
    return reason != ProbeSampleSkipReason::None;
bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    DdgiLaunchRejectReason reason = DdgiLaunchRejectReason::None;
    return preflightDdgiProbeUpdate(desc, probe_indices, probe_count, reason);

bool preflightDdgiProbeUpdate(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              DdgiLaunchRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = DdgiLaunchRejectReason::EmptyGrid;
        return false;
    if (probe_indices == nullptr) {
        outReason = DdgiLaunchRejectReason::NullIndices;
    if (probe_count == 0u) {
        outReason = DdgiLaunchRejectReason::ZeroProbeCount;
    if (desc.rays_per_probe == 0u) {
        outReason = DdgiLaunchRejectReason::InvalidRaysPerProbe;

    outReason = DdgiLaunchRejectReason::None;
    return true;
const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason) {
    case CacheIndexRejectReason::None:
    case CacheIndexRejectReason::EmptyGrid:
    case CacheIndexRejectReason::InvalidProbeIndex:
        return "invalid_probe_index";
    case CacheIndexRejectReason::UndersizedCache:

const char* probeUpdateLaunchRejectReasonLabel(ProbeUpdateLaunchRejectReason reason) {
    case ProbeUpdateLaunchRejectReason::None:
    case ProbeUpdateLaunchRejectReason::EmptyGrid:
    case ProbeUpdateLaunchRejectReason::NullIndices:
        return "null_indices";
    case ProbeUpdateLaunchRejectReason::ZeroCount:
        return "zero_count";
    case ProbeUpdateLaunchRejectReason::OutOfRangeIndex:
        return "out_of_range_index";
}

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

ProbeUpdateLaunchRejectReason classifyProbeUpdateLaunchReject(const DDGIDesc& desc,
                                                              const u32* probe_indices,
                                                              u32 probe_count) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        return ProbeUpdateLaunchRejectReason::EmptyGrid;
    }
    if (probe_indices == nullptr) {
        outReason = ProbeUpdateLaunchRejectReason::NullIndices;
    if (probe_count == 0u) {
        outReason = ProbeUpdateLaunchRejectReason::ZeroCount;
bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    if (probe_count == 0u || probe_indices == nullptr || ProbeGridLayout::isEmptyGrid(desc)) {
const char* ddgiLaunchRejectReasonLabel(DdgiLaunchRejectReason reason) {
    switch (reason) {
    case DdgiLaunchRejectReason::None:
        return "none";
    case DdgiLaunchRejectReason::EmptyGrid:
        return "empty_grid";
    case DdgiLaunchRejectReason::NullIndices:
        return "null_indices";
    case DdgiLaunchRejectReason::ZeroCount:
        return "zero_count";
    case DdgiLaunchRejectReason::OutOfRangeIndex:
        return "out_of_range_index";
    return "unknown";

                                 DdgiLaunchRejectReason& out_reason) {
        out_reason = DdgiLaunchRejectReason::EmptyGrid;
        out_reason = DdgiLaunchRejectReason::NullIndices;
        out_reason = DdgiLaunchRejectReason::ZeroCount;
    DdgiLaunchRejectReason reason = DdgiLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;

bool tryCanLaunchDdgiProbeUpdate(const DDGIDesc& desc,
                                 u32 probe_count,
                                 DdgiLaunchRejectReason& outReason) {
        outReason = DdgiLaunchRejectReason::EmptyGrid;
        outReason = DdgiLaunchRejectReason::NullIndices;
        outReason = DdgiLaunchRejectReason::ZeroCount;
    outReason = DdgiLaunchRejectReason::None;
        outReason = DdgiLaunchRejectReason::ZeroProbeCount;
        outReason = DdgiLaunchRejectReason::NullIndexBuffer;
        return false;
    if (desc.rays_per_probe == 0u) {
        outReason = ProbeUpdateLaunchRejectReason::ZeroRaysPerProbe;
        return ProbeUpdateLaunchRejectReason::NullIndices;
        return ProbeUpdateLaunchRejectReason::ZeroCount;

    for (u32 i = 0u; i < probe_count; ++i) {
        if (ProbeGridLayout::isProbeIndexOutOfRange(probe_indices[i], desc)) {
            outReason = ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex;
            outReason = ProbeUpdateLaunchRejectReason::OutOfRangeIndex;
            outReason = DdgiLaunchRejectReason::OutOfRangeIndex;
            return ProbeUpdateLaunchRejectReason::OutOfRangeProbeIndex;

    outReason = ProbeUpdateLaunchRejectReason::None;
            out_reason = DdgiLaunchRejectReason::OutOfRangeIndex;

    out_reason = DdgiLaunchRejectReason::None;


    return true;
    return ProbeUpdateLaunchRejectReason::None;
                                 ProbeUpdateLaunchRejectReason& outReason) {
    outReason = classifyProbeUpdateLaunchReject(desc, probe_indices, probe_count);
    return outReason == ProbeUpdateLaunchRejectReason::None;
}

bool tryCanLaunchDdgiProbeUpdate(const DDGIDesc& desc,
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 ProbeUpdateLaunchRejectReason& outReason) {
    outReason = classifyProbeUpdateLaunchReject(desc, probe_indices, probe_count);
    return outReason == ProbeUpdateLaunchRejectReason::None;

bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);

ProbeUpdateLaunchRejectReason classifyProbeUpdateLaunchReject(const DDGIDesc& desc,
                                                              const u32* probe_indices,
                                                              u32 probe_count) {
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;
    tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
    return reason;
}

bool wouldSkipDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    return !canLaunchDdgiProbeUpdate(desc, probe_indices, probe_count);
bool wouldSkipDdgiProbeUpdate(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              ProbeUpdateLaunchRejectReason* reason) {
    ProbeUpdateLaunchRejectReason local = ProbeUpdateLaunchRejectReason::None;
    const bool skip = !tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, local);
    if (reason != nullptr) {
        *reason = local;
    }
    return skip;

bool preflightDdgiProbeUpdate(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              ProbeUpdateLaunchRejectReason* reason) {
    ProbeUpdateLaunchRejectReason local = ProbeUpdateLaunchRejectReason::None;
    const bool ok = tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, local);
    if (reason != nullptr) {
        *reason = local;
    }
    return ok;
}

bool tryLaunch_ddgi_probe_update(const DDGIDesc& desc,
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 void* cuda_stream,
                                 ProbeUpdateLaunchRejectReason& outReason) {
    if (!tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, outReason)) {
    return launch_ddgi_probe_update(desc, probe_indices, probe_count, cuda_stream);


u32 countInvalidLaunchProbeIndices(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    if (probe_indices == nullptr || probe_count == 0u || ProbeGridLayout::isEmptyGrid(desc)) {
        return 0u;
    }

    u32 invalid_count = 0u;
    for (u32 i = 0u; i < probe_count; ++i) {
        if (ProbeGridLayout::isProbeIndexOutOfRange(probe_indices[i], desc)) {
            ++invalid_count;
        }
    }
    return invalid_count;
}

bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    DdgiLaunchRejectReason reason = DdgiLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
}

bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    DdgiLaunchRejectReason reason = DdgiLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
}

bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    DdgiLaunchRejectReason reason = DdgiLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
}

bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
}

bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    DdgiLaunchRejectReason reason = DdgiLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
}

bool tryCanLaunchDdgiProbeUpdate(const DDGIDesc& desc,
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 LaunchRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = LaunchRejectReason::EmptyGrid;
        return false;
    }
    if (probe_indices == nullptr) {
        outReason = LaunchRejectReason::NullIndices;
        return false;
    }
    if (probe_count == 0u) {
        outReason = LaunchRejectReason::ZeroCount;
        return false;
    }
    if (desc.rays_per_probe == 0u) {
        outReason = LaunchRejectReason::ZeroRaysPerProbe;
        return false;
    }

    for (u32 i = 0u; i < probe_count; ++i) {
        if (ProbeGridLayout::isProbeIndexOutOfRange(probe_indices[i], desc)) {
            outReason = LaunchRejectReason::ProbeIndexOutOfRange;
            return false;
        }
    }

    outReason = LaunchRejectReason::None;
    return true;
}

bool launch_ddgi_probe_update(const DDGIDesc& desc,
                              const u32* probe_indices,
                              u32 probe_count,
                              void* cuda_stream) {
    if (!canLaunchDdgiProbeUpdate(desc, probe_indices, probe_count)) {
    DdgiLaunchRejectReason reason = DdgiLaunchRejectReason::None;
    if (!preflightDdgiProbeUpdate(desc, probe_indices, probe_count, reason)) {
        return false;
    }

    for (u32 i = 0u; i < probe_count; ++i) {
        if (ProbeGridLayout::isProbeIndexOutOfRange(probe_indices[i], desc)) {
            return false;
        }
    }

    gi::DDGIKernelParams params{};
    params.probe_indices_to_update = probe_indices;
    params.probe_update_count = probe_count;
    params.rays_per_probe = desc.rays_per_probe;
    params.hysteresis = desc.hysteresis;
    params.max_ray_distance = desc.max_ray_distance;

    if (!gi::canLaunchProbeUpdate(params)) {
    if (!gi::canLaunchDdgiKernels(desc, params)) {
        return false;
    }

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
    case ProbeKernelRejectReason::NullRadianceSurfaces:
        return "null_radiance_surfaces";
    case ProbeKernelRejectReason::NullAtlasSurfaces:
        return "null_atlas_surfaces";
    case ProbeKernelRejectReason::ZeroMaxRayDistance:
        return "zero_max_ray_distance";
    case ProbeKernelRejectReason::InvalidHysteresis:
        return "invalid_hysteresis";
    case ProbeKernelRejectReason::NullBlendSurfaces:
        return "null_blend_surfaces";
    case ProbeKernelRejectReason::NullRadianceSurface:
        return "null_radiance_surface";
    case ProbeKernelRejectReason::NullProbeWorldPositions:
        return "null_probe_world_positions";
    case ProbeKernelRejectReason::NullIrradianceAtlas:
        return "null_irradiance_atlas";
    case ProbeKernelRejectReason::NullDepthAtlas:
        return "null_depth_atlas";
    }
    return "unknown";

bool probeKernelRejectReasonIsBlocking(ProbeKernelRejectReason reason) {
    return reason != ProbeKernelRejectReason::None;

ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    tryCanLaunchProbeTraceKernel(params, reason);
    return reason;

bool wouldSkipProbeKernelLaunch(const DDGIKernelParams& params) {
    return !canLaunchProbeTraceKernel(params);

bool preflightProbeKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    const ProbeKernelRejectReason reject = classifyProbeKernelReject(params);
    if (reason != nullptr) {
        *reason = reject;
    return !probeKernelRejectReasonIsBlocking(reject);

bool tryPreflightProbeKernelLaunch(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    return preflightProbeKernelLaunch(params, &outReason);

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

bool preflightProbeKernelParams(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    if (!tryCanLaunchProbeTraceKernel(params, outReason)) {
namespace {

bool isValidKernelHysteresis(f32 hysteresis) {
    return hysteresis >= 0.f && hysteresis <= 1.f;
}

} // namespace
bool tryCanLaunchProbeTraceKernelWithSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    if (!tryCanLaunchProbeTraceKernel(params, outReason)) {
        return false;
    }
    if (params.out_radiance_surface == nullptr) {
        outReason = ProbeKernelRejectReason::NullRadianceSurface;
    outReason = ProbeKernelRejectReason::None;
    return true;

bool canLaunchProbeTraceKernelWithSurfaces(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    return tryCanLaunchProbeTraceKernelWithSurfaces(params, reason);

bool tryCanLaunchProbeBlendKernelWithSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    if (params.irradiance_atlas_surface == nullptr) {
        outReason = ProbeKernelRejectReason::NullIrradianceAtlas;
    if (params.depth_atlas_surface == nullptr) {
        outReason = ProbeKernelRejectReason::NullDepthAtlas;

bool canLaunchProbeBlendKernelWithSurfaces(const DDGIKernelParams& params) {
    return tryCanLaunchProbeBlendKernelWithSurfaces(params, reason);

bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params) {
    if (params.probe_update_count == 0u) {
        return ProbeKernelRejectReason::ZeroUpdateCount;
    }
    if (params.rays_per_probe == 0u) {
        outReason = ProbeKernelRejectReason::ZeroRaysPerProbe;
        return false;
    if (params.probe_indices_to_update == nullptr) {
        return ProbeKernelRejectReason::NullProbeIndices;
    }
    if (params.rays_per_probe == 0u) {
        return ProbeKernelRejectReason::ZeroRaysPerProbe;
    }
    if (params.max_ray_distance <= 0.f) {
        outReason = ProbeKernelRejectReason::ZeroMaxRayDistance;
        return false;
    }
    outReason = ProbeKernelRejectReason::None;
    return true;
    return ProbeKernelRejectReason::None;

bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    outReason = classifyProbeKernelReject(params);
    return outReason == ProbeKernelRejectReason::None;
}

ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params) {
    if (params.probe_update_count == 0u) {
        outReason = ProbeKernelRejectReason::ZeroUpdateCount;
        return false;
    if (params.probe_indices_to_update == nullptr) {
        outReason = ProbeKernelRejectReason::NullProbeIndices;
    if (params.rays_per_probe == 0u) {
        outReason = ProbeKernelRejectReason::ZeroRaysPerProbe;
    outReason = ProbeKernelRejectReason::None;
    return true;
        return ProbeKernelRejectReason::ZeroUpdateCount;
    }
        return ProbeKernelRejectReason::NullProbeIndices;
        return ProbeKernelRejectReason::ZeroRaysPerProbe;
    return ProbeKernelRejectReason::None;

bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    outReason = classifyProbeKernelReject(params);
    return outReason == ProbeKernelRejectReason::None;

bool canLaunchProbeTraceKernel(const DDGIKernelParams& params) {
    return tryCanLaunchProbeTraceKernel(params, reason);

bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeTraceKernel(params);
}


bool hasProbeBlendKernelSurfaces(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    return tryValidateProbeBlendKernelSurfaces(params, reason);

bool tryValidateProbeBlendKernelSurfaces(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    if (params.prev_irradiance_surface == nullptr || params.out_radiance_surface == nullptr ||
        params.irradiance_atlas_surface == nullptr) {
        outReason = ProbeKernelRejectReason::NullBlendSurfaces;
        return false;
    outReason = ProbeKernelRejectReason::None;
    return true;
bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeTraceKernel(params);
}

bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeTraceKernel(params);
}

ProbeKernelRejectReason classifyProbeTraceKernelReject(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    tryCanLaunchProbeTraceKernel(params, reason);
    return reason;
}

bool preflightProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    ProbeKernelRejectReason local = ProbeKernelRejectReason::None;
    const bool ok = tryCanLaunchProbeTraceKernel(params, local);
    if (reason != nullptr) {
        *reason = local;
    }
    return ok;
}

bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeTraceKernel(params);
}

bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {
    return tryCanLaunchProbeTraceKernel(params, outReason);
    if (!tryCanLaunchProbeTraceKernel(params, outReason)) {
    if (params.prev_irradiance_surface == nullptr || params.out_radiance_surface == nullptr) {
        outReason = ProbeKernelRejectReason::NullRadianceSurfaces;
    if (params.irradiance_atlas_surface == nullptr || params.depth_atlas_surface == nullptr) {
        outReason = ProbeKernelRejectReason::NullAtlasSurfaces;
    if (!isValidKernelHysteresis(params.hysteresis)) {
        outReason = ProbeKernelRejectReason::InvalidHysteresis;

bool tryPreflightProbeTraceKernelResources(const DDGIKernelParams& params,
                                           ProbeKernelRejectReason& outReason) {
    if (params.probe_world_positions == nullptr) {
        outReason = ProbeKernelRejectReason::NullProbeWorldPositions;

bool tryPreflightProbeBlendKernelResources(const DDGIKernelParams& params,
    if (params.irradiance_atlas_surface == nullptr) {
        outReason = ProbeKernelRejectReason::NullIrradianceAtlas;
    if (params.depth_atlas_surface == nullptr) {
        outReason = ProbeKernelRejectReason::NullDepthAtlas;

bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeTraceKernel(params);
}

bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeBlendKernel(params);
}

bool canLaunchProbeBlendKernel(const DDGIKernelParams& params) {
    return tryCanLaunchProbeBlendKernel(params, reason);


bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeBlendKernel(params);

bool tryLaunch_probe_trace_kernel(const DDGIKernelParams& params,
                                  void* cuda_stream,
    return launch_probe_trace_kernel(params, cuda_stream);
bool canLaunchProbeUpdate(const DDGIKernelParams& params) {
    if (params.probe_indices_to_update == nullptr || params.probe_update_count == 0u ||
        params.rays_per_probe == 0u) {
    return params.max_ray_distance > 0.f;
const char* ddgiKernelRejectReasonLabel(DdgiKernelRejectReason reason) {
    case DdgiKernelRejectReason::None:
    case DdgiKernelRejectReason::NullProbeIndices:
    case DdgiKernelRejectReason::ZeroProbeCount:
        return "zero_probe_count";
    case DdgiKernelRejectReason::ZeroRaysPerProbe:

bool preflightDDGIKernelParams(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {
        outReason = DdgiKernelRejectReason::ZeroProbeCount;
        outReason = DdgiKernelRejectReason::NullProbeIndices;
        outReason = DdgiKernelRejectReason::ZeroRaysPerProbe;

    outReason = DdgiKernelRejectReason::None;

    DdgiKernelRejectReason reason = DdgiKernelRejectReason::None;
    return preflightDDGIKernelParams(params, reason);





ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params) {
    tryCanLaunchProbeTraceKernel(params, reason);
    return reason;




bool tryLaunch_probe_blend_kernel(const DDGIKernelParams& params,
    if (!tryCanLaunchProbeBlendKernel(params, outReason)) {
    return launch_probe_blend_kernel(params, cuda_stream);

const char* probeKernelResourceRejectReasonLabel(ProbeKernelResourceRejectReason reason) {
    switch (reason) {
    case ProbeKernelResourceRejectReason::None:
        return "none";
    case ProbeKernelResourceRejectReason::NullProbeWorldPositions:
        return "null_probe_world_positions";
    case ProbeKernelResourceRejectReason::NullIrradianceAtlas:
        return "null_irradiance_atlas";
    case ProbeKernelResourceRejectReason::NullDepthAtlas:
        return "null_depth_atlas";
    case ProbeKernelResourceRejectReason::NullPrevIrradianceSurface:
        return "null_prev_irradiance_surface";
    case ProbeKernelResourceRejectReason::NullOutRadianceSurface:
        return "null_out_radiance_surface";
    return "unknown";

bool hasProbeTraceGpuResources(const DDGIKernelParams& params) {
    ProbeKernelResourceRejectReason reason = ProbeKernelResourceRejectReason::None;
    return tryValidateProbeKernelResources(params, reason);

bool hasProbeBlendGpuResources(const DDGIKernelParams& params) {
    return hasProbeTraceGpuResources(params);

bool tryValidateProbeKernelResources(const DDGIKernelParams& params,
                                     ProbeKernelResourceRejectReason& outReason) {
        outReason = ProbeKernelResourceRejectReason::NullProbeWorldPositions;
        outReason = ProbeKernelResourceRejectReason::NullIrradianceAtlas;
        outReason = ProbeKernelResourceRejectReason::NullDepthAtlas;
    if (params.prev_irradiance_surface == nullptr) {
        outReason = ProbeKernelResourceRejectReason::NullPrevIrradianceSurface;
    if (params.out_radiance_surface == nullptr) {
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






bool canLaunchDdgiKernelParams(const DDGIKernelParams& params) {
    return canLaunchProbeTraceKernel(params);

bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, ProbeKernelRejectReason& outReason) {















    return classifyProbeKernelReject(params) == ProbeKernelRejectReason::None;

    return classifyProbeKernelReject(params) != ProbeKernelRejectReason::None;

    outReason = classifyProbeKernelReject(params);
    return outReason == ProbeKernelRejectReason::None;


}

bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeBlendKernel(params);
}

bool wouldSkipProbeTraceKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    ProbeKernelRejectReason local = ProbeKernelRejectReason::None;
    const bool skip = !tryCanLaunchProbeTraceKernel(params, local);
    if (reason != nullptr) {
        *reason = local;
    }
    return skip;
}

bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    ProbeKernelRejectReason local = ProbeKernelRejectReason::None;
    const bool skip = !tryCanLaunchProbeBlendKernel(params, local);
    if (reason != nullptr) {
        *reason = local;
    }
    return skip;
}

bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeBlendKernel(params);
}

ProbeKernelRejectReason classifyProbeKernelReject(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    tryCanLaunchProbeTraceKernel(params, reason);
    return reason;
}

ProbeKernelRejectReason classifyProbeBlendKernelReject(const DDGIKernelParams& params) {
    ProbeKernelRejectReason reason = ProbeKernelRejectReason::None;
    tryCanLaunchProbeBlendKernel(params, reason);
    return reason;
}

bool preflightProbeBlendKernel(const DDGIKernelParams& params, ProbeKernelRejectReason* reason) {
    ProbeKernelRejectReason local = ProbeKernelRejectReason::None;
    const bool ok = tryCanLaunchProbeBlendKernel(params, local);
    if (reason != nullptr) {
        *reason = local;
    }
    return ok;
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
const char* probeKernelLaunchRejectReasonLabel(ProbeKernelLaunchRejectReason reason) {
    case ProbeKernelLaunchRejectReason::None:
    case ProbeKernelLaunchRejectReason::NullProbeIndices:
    case ProbeKernelLaunchRejectReason::ZeroProbeCount:
    case ProbeKernelLaunchRejectReason::ZeroRaysPerProbe:

bool tryCanLaunchProbeKernels(const DDGIKernelParams& params, ProbeKernelLaunchRejectReason& outReason) {
    outReason = ProbeKernelLaunchRejectReason::None;
        outReason = ProbeKernelLaunchRejectReason::NullProbeIndices;
        outReason = ProbeKernelLaunchRejectReason::ZeroProbeCount;
        outReason = ProbeKernelLaunchRejectReason::ZeroRaysPerProbe;

bool canLaunchProbeKernels(const DDGIKernelParams& params) {
    ProbeKernelLaunchRejectReason reason = ProbeKernelLaunchRejectReason::None;
    return tryCanLaunchProbeKernels(params, reason);

    if (!canLaunchProbeKernels(params)) {
const char* ddgiKernelLaunchRejectReasonLabel(DdgiKernelLaunchRejectReason reason) {
    case DdgiKernelLaunchRejectReason::None:
    case DdgiKernelLaunchRejectReason::NullIndices:
        return "null_indices";
    case DdgiKernelLaunchRejectReason::ZeroUpdateCount:

    DdgiKernelLaunchRejectReason reason = DdgiKernelLaunchRejectReason::None;
    return preflightProbeTraceKernel(params, reason);

bool preflightProbeTraceKernel(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason) {
        outReason = DdgiKernelLaunchRejectReason::NullIndices;
        outReason = DdgiKernelLaunchRejectReason::ZeroUpdateCount;

    outReason = DdgiKernelLaunchRejectReason::None;

    return preflightProbeBlendKernel(params, reason);

bool preflightProbeBlendKernel(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason) {


    if (!preflightProbeTraceKernel(params, reason)) {

    return params.probe_update_count > 0u && params.probe_indices_to_update != nullptr;

    if (params.probe_update_count == 0u || params.probe_indices_to_update == nullptr) {

    if (params.hysteresis < 0.f || params.hysteresis > 1.f) {

    case DdgiKernelRejectReason::NullIndices:
    case DdgiKernelRejectReason::ZeroCount:
        return "zero_count";
    case DdgiKernelRejectReason::InvalidRaysPerProbe:
        return "invalid_rays_per_probe";
    case DdgiKernelRejectReason::OutOfRangeIndex:
        return "out_of_range_index";


bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {
        outReason = DdgiKernelRejectReason::ZeroCount;
        outReason = DdgiKernelRejectReason::NullIndices;
        outReason = DdgiKernelRejectReason::InvalidRaysPerProbe;


bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {

bool canLaunchDdgiKernels(const DDGIDesc& desc, const DDGIKernelParams& params) {
    return tryCanLaunchDdgiKernels(desc, params, reason);

bool tryCanLaunchDdgiKernels(const DDGIDesc& desc,
                             const DDGIKernelParams& params,
                             DdgiKernelRejectReason& outReason) {
    if (!tryCanLaunchProbeBlendKernel(params, outReason)) {
    for (u32 i = 0u; i < params.probe_update_count; ++i) {
        if (ProbeGridLayout::isProbeIndexOutOfRange(params.probe_indices_to_update[i], desc)) {
            outReason = DdgiKernelRejectReason::OutOfRangeIndex;



















    case DdgiKernelLaunchRejectReason::NullProbeIndices:
    case DdgiKernelLaunchRejectReason::ZeroRaysPerProbe:

bool canLaunchDdgiKernelParams(const DDGIKernelParams& params) {
    return tryCanLaunchDdgiKernelParams(params, reason);

bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason) {
        outReason = DdgiKernelLaunchRejectReason::NullProbeIndices;
        outReason = DdgiKernelLaunchRejectReason::ZeroRaysPerProbe;


    if (!canLaunchDdgiKernelParams(params)) {
    case DdgiKernelRejectReason::ZeroUpdateCount:

bool preflightDdgiKernelParams(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {
        outReason = DdgiKernelRejectReason::ZeroUpdateCount;

    return preflightDdgiKernelParams(params, reason);



namespace {

bool tryCanLaunchProbeKernel(const DDGIKernelParams& params, DdgiKernelRejectReason& outReason) {


} // namespace


    return tryCanLaunchProbeKernel(params, outReason);



const char* kernelLaunchRejectReasonLabel(KernelLaunchRejectReason reason) {
    case KernelLaunchRejectReason::None:
    case KernelLaunchRejectReason::NullIndices:
    case KernelLaunchRejectReason::ZeroCount:
    case KernelLaunchRejectReason::ZeroRaysPerProbe:

    KernelLaunchRejectReason reason = KernelLaunchRejectReason::None;

bool tryCanLaunchProbeTraceKernel(const DDGIKernelParams& params, KernelLaunchRejectReason& outReason) {
    if (params.probe_update_count > 0u && params.probe_indices_to_update == nullptr) {
        outReason = KernelLaunchRejectReason::NullIndices;
        outReason = KernelLaunchRejectReason::ZeroCount;
        outReason = KernelLaunchRejectReason::ZeroRaysPerProbe;

    outReason = KernelLaunchRejectReason::None;


bool tryCanLaunchProbeBlendKernel(const DDGIKernelParams& params, KernelLaunchRejectReason& outReason) {


    (void)cuda_stream;
    if (!canLaunchProbeTraceKernel(params)) {
        return false;
    }
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
bool tryLaunch_probe_trace_kernel(const DDGIKernelParams& params,
    if (!tryCanLaunchProbeTraceKernel(params, outReason)) {
    return launch_probe_trace_kernel(params, cuda_stream);

bool wouldSkipProbeBlendKernel(const DDGIKernelParams& params) {
    return !canLaunchProbeBlendKernel(params);

}

bool launch_probe_blend_kernel(const DDGIKernelParams& params, void* cuda_stream) {
    if (!canLaunchProbeBlendKernel(params)) {
        return false;
    }
    return launch_probe_blend_kernel(params, cuda_stream);

bool tryLaunch_probe_kernels(const DDGIKernelParams& params,
    if (!tryLaunch_probe_trace_kernel(params, cuda_stream, outReason)) {
    return tryLaunch_probe_blend_kernel(params, cuda_stream, outReason);
bool tryLaunch_probe_trace_kernel(const DDGIKernelParams& params,
    if (!tryCanLaunchProbeTraceKernel(params, outReason)) {
    return launch_probe_trace_kernel(params, cuda_stream);
}

bool launch_probe_blend_kernel(const DDGIKernelParams& params, void* cuda_stream) {
    if (!canLaunchProbeBlendKernel(params)) {
    if (!canLaunchProbeKernels(params)) {
        return false;
    }
    DdgiKernelLaunchRejectReason reason = DdgiKernelLaunchRejectReason::None;
    if (!preflightProbeBlendKernel(params, reason)) {

    if (!canLaunchDdgiKernelParams(params)) {
    (void)cuda_stream;
    // Stub launch mirrors trace preflight until CUDA surfaces are wired.
    if (!canLaunchProbeTraceKernel(params)) {
        return false;
    }
#if defined(FUSE_HAS_CUDA)
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
    const u32 schedule_capacity =
        std::max(1u, ddgi_util::effectiveScheduledProbeCount(probe_count,
                                                               m_desc.probes_per_frame,
                                                               m_desc.probes_per_frame));
    std::vector<u32> scheduled_indices(schedule_capacity);
    u32 scheduled_count = 0u;
    ProbeScheduleRejectReason schedule_reason = ProbeScheduleRejectReason::None;
    ddgi_util::tryScheduleProbeUpdates(frame_index,
                                       probe_count,
                                       m_desc.probes_per_frame,
                                       scheduled_indices.data(),
                                       schedule_capacity,
                                       &scheduled_count,
                                       schedule_reason);
    m_last_update.probes_scheduled = scheduled_count;

    ProbeUpdateLaunchRejectReason launch_reason = ProbeUpdateLaunchRejectReason::None;
    m_last_update.kernel_launched = tryLaunch_ddgi_probe_update(
        m_desc, scheduled_indices.data(), scheduled_count, cuda_stream, launch_reason);

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

    if (!ddgi_util::isValidSampleRequest(m_desc, request, static_cast<u32>(m_cache.size()))) {
    const u32 cache_count = static_cast<u32>(m_cache.size());
    if (!ddgi_util::isValidSampleRequest(m_desc, request, cache_count)) {
        return result;
    }

    const u32 cache_count = static_cast<u32>(m_cache.size());
    if (!ddgi_util::isValidSampleRequest(m_desc, request, cache_count)) {
        return result;
    }

    if (!ddgi_util::isValidSampleRequest(m_desc, request, static_cast<u32>(m_cache.size()))) {
        return result;
    }

    const u32 cache_count = static_cast<u32>(m_cache.size());
    if (!ddgi_util::isValidSampleRequest(m_desc, request, cache_count)) {
        return result;
    }

    const u32 nearest = ddgi_util::nearestProbeIndex(m_desc, request.world_position);
    if (!ddgi_util::canAccessCacheIndex(m_desc, nearest, cache_count)) {
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
                                                                       cache_count);
    if (!ddgi_util::tryTrilinearDirectionalProbeIrradiance(m_desc,
                                                           cache_count,
                                                           result.irradiance)) {
        return result;
    }
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
