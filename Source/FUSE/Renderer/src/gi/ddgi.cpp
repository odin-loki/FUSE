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

bool ProbeGridLayout::isValidProbeSampleCoords(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
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
    if (coords.x0 > coords.x1 || coords.y0 > coords.y1 || coords.z0 > coords.z1) {
        return false;
    }

    return coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f && coords.tz >= 0.f &&
           coords.tz <= 1.f;
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

bool ProbeGridLayout::tryClampProbeSampleCoords(const DDGIDesc& desc, ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return false;
    }

    clampProbeSampleCoords(desc, coords);
    return true;
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

    return coords.tx >= 0.f && coords.tx <= 1.f && coords.ty >= 0.f && coords.ty <= 1.f && coords.tz >= 0.f &&
           coords.tz <= 1.f;
}

bool ProbeGridLayout::isProbeSampleCoordsOutOfRange(const DDGIDesc& desc, const ProbeSampleCoords& coords) {
    if (isEmptyGrid(desc)) {
        return true;
    }
    return !areProbeSampleCoordsInBounds(desc, coords);
}

bool ProbeGridLayout::buildProbeSampleCoords(const DDGIDesc& desc,
                                             const fuse::math::Vec3& world_position,
                                             ProbeSampleCoords& out_coords) {
    if (isEmptyGrid(desc)) {
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

bool isCacheIndexValid(const DDGIDesc& desc, u32 probe_index, u32 cache_count) {
    return ProbeGridLayout::isValidProbeIndex(desc, probe_index) && probe_index < cache_count;
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
        return false;
    }
    if (probe_index >= cache_count) {
        outReason = CacheIndexRejectReason::UndersizedCache;
        return false;
    }

    outReason = CacheIndexRejectReason::None;
    return true;
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

void scheduleProbeUpdates(u32 frame_index,
                          u32 probe_count,
                          u32 probes_per_frame,
                          u32* out_indices,
                          u32 max_indices,
                          u32* out_count) {
    if (out_indices == nullptr || out_count == nullptr || probe_count == 0u || max_indices == 0u) {
        if (out_count != nullptr) {
            *out_count = 0u;
        }
        return;
    }

    const u32 count = std::min(probes_per_frame, std::min(probe_count, max_indices));
    const u32 start = (frame_index * probes_per_frame) % probe_count;
    for (u32 i = 0; i < count; ++i) {
        out_indices[i] = (start + i) % probe_count;
    }
    *out_count = count;
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

fuse::math::Vec3 trilinearProbeIrradiance(const DDGIDesc& desc,
                                          const fuse::math::Vec3& world_position,
                                          const IrradianceCacheEntry* cache,
                                          u32 cache_count) {
    if (cache == nullptr || !canSampleProbeGrid(desc) || !isCacheSizedForGrid(desc, cache_count)) {
        return {};
    }

    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords)) {
        return {};
    }
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        return {};
    }

    const auto sample_probe = [&](u32 x, u32 y, u32 z) -> fuse::math::Vec3 {
        const ProbeGridCoord probe_coord{x, y, z};
        const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, probe_coord);
        if (!isCacheIndexValid(desc, index, cache_count)) {
            return {};
        }
        return cache[index].irradiance;
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

fuse::math::Vec3 trilinearDirectionalProbeIrradiance(const DDGIDesc& desc,
                                                     const fuse::math::Vec3& world_position,
                                                     const fuse::math::Vec3& direction,
                                                     const IrradianceCacheEntry* cache,
                                                     u32 cache_count) {
    if (cache == nullptr || !canSampleProbeGrid(desc) || !isCacheSizedForGrid(desc, cache_count)) {
        return {};
    }

    const fuse::math::Vec3 sample_direction = DdgiIrradianceEncoding::resolveSampleDirection(direction);

    ProbeSampleCoords coords{};
    if (!ProbeGridLayout::buildProbeSampleCoords(desc, world_position, coords)) {
        return {};
    }
    if (!ProbeGridLayout::isValidProbeSampleCoords(desc, coords)) {
        return {};
    }

    const auto sample_probe = [&](u32 x, u32 y, u32 z) -> fuse::math::Vec3 {
        const ProbeGridCoord probe_coord{x, y, z};
        const u32 index = ProbeGridLayout::probeIndexFromCoord(desc, probe_coord);
        if (!isCacheIndexValid(desc, index, cache_count)) {
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
    return lerpIrradiance(c0, c1, coords.tz);
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

const char* cacheIndexRejectReasonLabel(CacheIndexRejectReason reason) {
    switch (reason) {
    case CacheIndexRejectReason::None:
        return "none";
    case CacheIndexRejectReason::EmptyGrid:
        return "empty_grid";
    case CacheIndexRejectReason::InvalidProbeIndex:
        return "invalid_probe_index";
    case CacheIndexRejectReason::UndersizedCache:
        return "undersized_cache";
    }
    return "unknown";
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
    case ProbeUpdateLaunchRejectReason::OutOfRangeIndex:
        return "out_of_range_index";
    }
    return "unknown";
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

bool canLaunchDdgiProbeUpdate(const DDGIDesc& desc, const u32* probe_indices, u32 probe_count) {
    ProbeUpdateLaunchRejectReason reason = ProbeUpdateLaunchRejectReason::None;
    return tryCanLaunchDdgiProbeUpdate(desc, probe_indices, probe_count, reason);
}

bool tryCanLaunchDdgiProbeUpdate(const DDGIDesc& desc,
                                 const u32* probe_indices,
                                 u32 probe_count,
                                 ProbeUpdateLaunchRejectReason& outReason) {
    if (ProbeGridLayout::isEmptyGrid(desc)) {
        outReason = ProbeUpdateLaunchRejectReason::EmptyGrid;
        return false;
    }
    if (probe_count == 0u) {
        outReason = ProbeUpdateLaunchRejectReason::ZeroCount;
        return false;
    }
    if (probe_indices == nullptr) {
        outReason = ProbeUpdateLaunchRejectReason::NullIndices;
        return false;
    }

    for (u32 i = 0u; i < probe_count; ++i) {
        if (ProbeGridLayout::isProbeIndexOutOfRange(probe_indices[i], desc)) {
            outReason = ProbeUpdateLaunchRejectReason::OutOfRangeIndex;
            return false;
        }
    }

    outReason = ProbeUpdateLaunchRejectReason::None;
    return true;
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

const char* ddgiKernelLaunchRejectReasonLabel(DdgiKernelLaunchRejectReason reason) {
    switch (reason) {
    case DdgiKernelLaunchRejectReason::None:
        return "none";
    case DdgiKernelLaunchRejectReason::ZeroUpdateCount:
        return "zero_update_count";
    case DdgiKernelLaunchRejectReason::NullProbeIndices:
        return "null_probe_indices";
    case DdgiKernelLaunchRejectReason::ZeroRaysPerProbe:
        return "zero_rays_per_probe";
    }
    return "unknown";
}

bool canLaunchDdgiKernelParams(const DDGIKernelParams& params) {
    DdgiKernelLaunchRejectReason reason = DdgiKernelLaunchRejectReason::None;
    return tryCanLaunchDdgiKernelParams(params, reason);
}

bool tryCanLaunchDdgiKernelParams(const DDGIKernelParams& params, DdgiKernelLaunchRejectReason& outReason) {
    if (params.probe_update_count == 0u) {
        outReason = DdgiKernelLaunchRejectReason::ZeroUpdateCount;
        return false;
    }
    if (params.probe_indices_to_update == nullptr) {
        outReason = DdgiKernelLaunchRejectReason::NullProbeIndices;
        return false;
    }
    if (params.rays_per_probe == 0u) {
        outReason = DdgiKernelLaunchRejectReason::ZeroRaysPerProbe;
        return false;
    }

    outReason = DdgiKernelLaunchRejectReason::None;
    return true;
}

bool launch_probe_trace_kernel(const DDGIKernelParams& params, void* cuda_stream) {
    if (!canLaunchDdgiKernelParams(params)) {
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

bool launch_probe_blend_kernel(const DDGIKernelParams& params, void* cuda_stream) {
    if (!canLaunchDdgiKernelParams(params)) {
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
