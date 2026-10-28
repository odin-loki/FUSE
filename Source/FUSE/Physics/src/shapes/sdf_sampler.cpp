#include <fuse/physics/shapes/sdf_sampler.hpp>

#include <fuse/scene/svo.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace fuse::physics {

vec3 SdfSampler::gradient(vec3 p) const {
    const f32 h = gradientStep();
    return {distance(p + vec3{h, 0.f, 0.f}) - distance(p - vec3{h, 0.f, 0.f}),
            distance(p + vec3{0.f, h, 0.f}) - distance(p - vec3{0.f, h, 0.f}),
            distance(p + vec3{0.f, 0.f, h}) - distance(p - vec3{0.f, 0.f, h})};
}

AnalyticSdf::AnalyticSdf(AnalyticSdfKind kind, vec3 params, f32 rounding)
    : m_kind(kind), m_params(params), m_rounding(std::max(rounding, 0.f)) {}

f32 AnalyticSdf::distance(vec3 p) const {
    switch (m_kind) {
    case AnalyticSdfKind::Sphere:
        return p.length() - m_params.x;
    case AnalyticSdfKind::Box: {
        const vec3 h{std::max(m_params.x - m_rounding, 0.f), std::max(m_params.y - m_rounding, 0.f),
                     std::max(m_params.z - m_rounding, 0.f)};
        const vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
        const vec3 outside{std::max(q.x, 0.f), std::max(q.y, 0.f), std::max(q.z, 0.f)};
        return outside.length() + std::min(std::max(q.x, std::max(q.y, q.z)), 0.f) - m_rounding;
    }
    case AnalyticSdfKind::Capsule: {
        const f32 y = std::clamp(p.y, -m_params.y, m_params.y);
        return (p - vec3{0.f, y, 0.f}).length() - m_params.x;
    }
    case AnalyticSdfKind::Torus: {
        const f32 ring = std::sqrt(p.x * p.x + p.z * p.z) - m_params.x;
        return std::sqrt(ring * ring + p.y * p.y) - m_params.y;
    }
    case AnalyticSdfKind::Cylinder: {
        const f32 dx = std::sqrt(p.x * p.x + p.z * p.z) - m_params.x;
        const f32 dy = std::fabs(p.y) - m_params.y;
        const f32 ox = std::max(dx, 0.f);
        const f32 oy = std::max(dy, 0.f);
        return std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.f);
    }
    }
    return p.length() - m_params.x;
}

vec3 AnalyticSdf::boundsMax() const {
    switch (m_kind) {
    case AnalyticSdfKind::Sphere:
        return {m_params.x, m_params.x, m_params.x};
    case AnalyticSdfKind::Box:
        return m_params;
    case AnalyticSdfKind::Capsule:
        return {m_params.x, m_params.x + m_params.y, m_params.x};
    case AnalyticSdfKind::Torus:
        return {m_params.x + m_params.y, m_params.y, m_params.x + m_params.y};
    case AnalyticSdfKind::Cylinder:
        return {m_params.x, m_params.y, m_params.x};
    }
    return m_params;
}

vec3 AnalyticSdf::boundsMin() const {
    return boundsMax() * -1.f;
}

vec3 AnalyticSdf::gradient(vec3 p) const {
    // Central differences at a fixed step are exact enough for these fields and keep the normal
    // continuous across the box's face / edge regions.
    const f32 h = 1e-4f * std::max(1.f, std::max(m_params.x, std::max(m_params.y, m_params.z)));
    return {distance(p + vec3{h, 0.f, 0.f}) - distance(p - vec3{h, 0.f, 0.f}),
            distance(p + vec3{0.f, h, 0.f}) - distance(p - vec3{0.f, h, 0.f}),
            distance(p + vec3{0.f, 0.f, h}) - distance(p - vec3{0.f, 0.f, h})};
}

FunctionSdf::FunctionSdf(std::function<f32(vec3)> field, vec3 boundsMin, vec3 boundsMax, f32 gradientStep)
    : m_field(std::move(field)), m_min(boundsMin), m_max(boundsMax), m_step(gradientStep) {}

SvoSdfSampler::SvoSdfSampler(std::shared_ptr<const scene::SVO> svo, vec3 boundsMin, vec3 boundsMax)
    : m_svo(std::move(svo)), m_min(boundsMin), m_max(boundsMax), m_step(1e-3f) {
    if (m_svo != nullptr && m_svo->isInitialized()) {
        const scene::SVODesc& desc = m_svo->desc();
        const f32 leaf = desc.rootSize / static_cast<f32>(1u << desc.maxDepth);
        m_step = 0.25f * leaf;
    }
}

f32 SvoSdfSampler::distance(vec3 p) const {
    if (m_svo == nullptr || !m_svo->isInitialized()) {
        return 1e30f;
    }
    // Inside the SVO's narrow band (about half a voxel around written voxels) its interpolated samples are
    // the distance. Beyond it the samples saturate, so the exact distance to the nearest solid voxel box
    // (searched in a small neighbourhood through SVO::readBox) is used, capped at the search radius (a
    // lower bound, which is what casts and contacts need).
    const f32 banded = m_svo->sdfQuery(scene::vec3(p.x, p.y, p.z));
    const scene::SVODesc& desc = m_svo->desc();
    const f32 leaf = desc.rootSize / static_cast<f32>(1u << desc.maxDepth);
    if (banded < 0.45f * leaf) {
        return banded;
    }
    constexpr s32 kSearch = 8;
    const s32 cx = static_cast<s32>(std::floor((p.x - desc.origin.x) / leaf));
    const s32 cy = static_cast<s32>(std::floor((p.y - desc.origin.y) / leaf));
    const s32 cz = static_cast<s32>(std::floor((p.z - desc.origin.z) / leaf));
    thread_local std::vector<u32> region;
    const s32 edge = 2 * kSearch + 1;
    m_svo->readBox(scene::ivec3(cx - kSearch, cy - kSearch, cz - kSearch), scene::ivec3(edge, edge, edge), region);
    f32 best = static_cast<f32>(kSearch) * leaf;
    usize i = 0;
    for (s32 z = -kSearch; z <= kSearch; ++z) {
        for (s32 y = -kSearch; y <= kSearch; ++y) {
            for (s32 x = -kSearch; x <= kSearch; ++x, ++i) {
                if (region[i] == 0u) {
                    continue;
                }
                const f32 lx = desc.origin.x + static_cast<f32>(cx + x) * leaf;
                const f32 ly = desc.origin.y + static_cast<f32>(cy + y) * leaf;
                const f32 lz = desc.origin.z + static_cast<f32>(cz + z) * leaf;
                const f32 dx = std::max({lx - p.x, 0.f, p.x - (lx + leaf)});
                const f32 dy = std::max({ly - p.y, 0.f, p.y - (ly + leaf)});
                const f32 dz = std::max({lz - p.z, 0.f, p.z - (lz + leaf)});
                best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
        }
    }
    return std::max(best, banded);
}

namespace {

constexpr f32 kEmptyBrickDistance = 1e30f;

bool latticeCount(u32 nx, u32 ny, u32 nz, usize& count) {
    if (nx == 0u || ny == 0u || nz == 0u) {
        return false;
    }
    constexpr usize kMax = ~usize{0};
    const usize x = nx;
    const usize y = ny;
    const usize z = nz;
    if (x > kMax / y) {
        return false;
    }
    const usize xy = x * y;
    if (xy > kMax / z) {
        return false;
    }
    count = xy * z;
    return true;
}

// `g` is a lattice coordinate (0 at the first sample). A one-sample axis contributes that sample.
// The last sample is the t = 1 end of the previous cell, so the upper index stays in range.
void latticeAxis(double g, u32 n, u32& i0, u32& i1, f32& t) {
    if (n <= 1u) {
        i0 = 0u;
        i1 = 0u;
        t = 0.f;
        return;
    }
    const double maxIndex = static_cast<double>(n - 1u);
    const double c = std::clamp(g, 0.0, maxIndex);
    const auto i = static_cast<u32>(std::floor(c));
    if (i >= n - 1u) {
        i0 = n - 2u;
        i1 = n - 1u;
        t = 1.f;
        return;
    }
    i0 = i;
    i1 = i + 1u;
    t = static_cast<f32>(c - static_cast<double>(i));
}

} // namespace

AsdfBrickSampler::AsdfBrickSampler(vec3 origin, f32 voxelSize, u32 nx, u32 ny, u32 nz, std::vector<f32> samples) {
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) {
        return;
    }
    m_origin = origin;
    m_brickMin = origin;
    m_brickMax = origin;
    m_min = origin;
    m_max = origin;
    usize count = 0;
    if (!std::isfinite(voxelSize) || !(voxelSize > 0.f) || !latticeCount(nx, ny, nz, count) || samples.size() < count) {
        return;
    }
    m_voxel = voxelSize;
    m_nx = nx;
    m_ny = ny;
    m_nz = nz;
    m_samples = std::move(samples);
    m_samples.resize(count);
    const f32 ex = static_cast<f32>(nx - 1u) * voxelSize;
    const f32 ey = static_cast<f32>(ny - 1u) * voxelSize;
    const f32 ez = static_cast<f32>(nz - 1u) * voxelSize;
    if (!std::isfinite(ex) || !std::isfinite(ey) || !std::isfinite(ez)) {
        m_samples.clear();
        m_nx = 0u;
        m_ny = 0u;
        m_nz = 0u;
        m_voxel = 0.f;
        return;
    }
    m_brickMax = origin + vec3{ex, ey, ez};
    f32 mostNegative = 0.f;
    for (u32 z = 0; z < nz; ++z) {
        const bool zEdge = z == 0u || z + 1u == nz;
        for (u32 y = 0; y < ny; ++y) {
            const bool yEdge = y == 0u || y + 1u == ny;
            for (u32 x = 0; x < nx; ++x) {
                if (!zEdge && !yEdge && x != 0u && x + 1u != nx) {
                    continue;
                }
                const f32 s = at(x, y, z);
                if (std::isfinite(s) && s < mostNegative) {
                    mostNegative = s;
                }
            }
        }
    }
    const vec3 apron{-mostNegative, -mostNegative, -mostNegative};
    m_min = m_brickMin - apron;
    m_max = m_brickMax + apron;
    m_step = 0.25f * voxelSize;
    if (!(m_step > 0.f)) {
        m_step = 1e-3f;
    }
}

f32 AsdfBrickSampler::distance(vec3 p) const {
    return sample(p);
}

f32 AsdfBrickSampler::at(u32 x, u32 y, u32 z) const {
    return m_samples[(static_cast<usize>(z) * static_cast<usize>(m_ny) + static_cast<usize>(y)) * static_cast<usize>(m_nx) +
                     static_cast<usize>(x)];
}

f32 AsdfBrickSampler::interpolate(double gx, double gy, double gz) const {
    u32 x0 = 0u;
    u32 x1 = 0u;
    u32 y0 = 0u;
    u32 y1 = 0u;
    u32 z0 = 0u;
    u32 z1 = 0u;
    f32 tx = 0.f;
    f32 ty = 0.f;
    f32 tz = 0.f;
    latticeAxis(gx, m_nx, x0, x1, tx);
    latticeAxis(gy, m_ny, y0, y1, ty);
    latticeAxis(gz, m_nz, z0, z1, tz);
    f32 result = 0.f;
    for (u32 dz = 0u; dz < 2u; ++dz) {
        for (u32 dy = 0u; dy < 2u; ++dy) {
            for (u32 dx = 0u; dx < 2u; ++dx) {
                const f32 w = (dx != 0u ? tx : 1.f - tx) * (dy != 0u ? ty : 1.f - ty) * (dz != 0u ? tz : 1.f - tz);
                result += w * at(dx != 0u ? x1 : x0, dy != 0u ? y1 : y0, dz != 0u ? z1 : z0);
            }
        }
    }
    return result;
}

f32 AsdfBrickSampler::sample(vec3 p) const {
    if (m_samples.empty() || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        return kEmptyBrickDistance;
    }
    // Inside the brick `q == p` and the length term is zero. Outside, `q` is the nearest point of the
    // brick AABB and the interpolated value there is the edge sample.
    const vec3 q{std::clamp(p.x, m_brickMin.x, m_brickMax.x), std::clamp(p.y, m_brickMin.y, m_brickMax.y),
                 std::clamp(p.z, m_brickMin.z, m_brickMax.z)};
    const double inv = 1.0 / static_cast<double>(m_voxel);
    const double gx = (static_cast<double>(q.x) - static_cast<double>(m_origin.x)) * inv;
    const double gy = (static_cast<double>(q.y) - static_cast<double>(m_origin.y)) * inv;
    const double gz = (static_cast<double>(q.z) - static_cast<double>(m_origin.z)) * inv;
    const f32 value = interpolate(gx, gy, gz) + (p - q).length();
    return std::isfinite(value) ? value : kEmptyBrickDistance;
}

} // namespace fuse::physics
