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

} // namespace fuse::physics
