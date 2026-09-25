// WP-3.1 virtual shadow maps: clipmap placement and CPU reference drivers (see vsm_clipmap.hpp).
#include <fuse/renderer/shadow/vsm/vsm_clipmap.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/shadow/csm.hpp>
#include <fuse/renderer/shadow/vsm/vsm_kernel.hpp>

#include <cmath>
#include <cstring>

namespace fuse::renderer::vsm {

namespace {
constexpr f32 kMaxMarkRadiusTexels = 64.f; ///< keeps the marking footprint within 2 x 2 pages
} // namespace

bool VsmClipmap::init(const VsmClipmapDesc& desc) {
    m_valid = false;
    reset();
    if (desc.levels == 0u || desc.levels > kMaxLevels || !(desc.firstLevelExtent > 0.f) ||
        !(desc.firstLevelExtent < 1e20f) || !(desc.markRadiusTexels >= 0.f) || desc.markRadiusTexels > kMaxMarkRadiusTexels ||
        !(desc.texelsPerPixel > 0.f) || !(desc.texelsPerPixel < 1e6f) ||
        desc.lodBias < -static_cast<s32>(kMaxLevels) || desc.lodBias > static_cast<s32>(kMaxLevels)) {
        return false;
    }
    m_desc = desc;
    m_valid = true;
    return true;
}

void VsmClipmap::reset() {
    m_hasPrev = false;
    m_frame = 0;
    std::memset(m_prevRotation, 0, sizeof(m_prevRotation));
    std::memset(m_prevOrigin, 0, sizeof(m_prevOrigin));
    std::memset(m_prevDepthKey, 0, sizeof(m_prevDepthKey));
}

bool VsmClipmap::build(const VsmViewDesc& view, VsmFrameConstants& c) {
    if (!m_valid || view.depthWidth == 0u || view.depthHeight == 0u || !(view.pixelSpread >= 0.f) || !(view.pixelSpread < 1e6f)) {
        return false;
    }
    const math::Vec3 dir{view.lightDirection[0], view.lightDirection[1], view.lightDirection[2]};
    if (!(std::isfinite(dir.x) && std::isfinite(dir.y) && std::isfinite(dir.z)) ||
        CascadeLightSpaceLayout::isDegenerateLightDirection(dir)) {
        return false;
    }
    // The stabilised light basis of the CSM fallback: an origin-centred lookAt along the light.
    const math::Mat4 lightView = CascadeLightSpaceLayout::buildStableLightView(dir);
    f32 rot[9];
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 k = 0; k < 3u; ++k) {
            rot[r * 3u + k] = lightView.at(r, k);
        }
    }
    f32 camLight[3];
    for (u32 r = 0; r < 3u; ++r) {
        f64 s = 0.0;
        for (u32 k = 0; k < 3u; ++k) {
            s += static_cast<f64>(rot[r * 3u + k]) * static_cast<f64>(view.cameraPosition[k]);
        }
        camLight[r] = static_cast<f32>(s);
    }
    if (!(std::isfinite(camLight[0]) && std::isfinite(camLight[1]) && std::isfinite(camLight[2]))) {
        return false;
    }
    const u32 levels = m_desc.levels;
    const f32 pw0 = m_desc.firstLevelExtent / static_cast<f32>(kPagesPerAxis);
    const f32 inv0 = 1.f / pw0;
    VsmLevelConstants level[kMaxLevels]{};
    const bool first = !m_hasPrev;
    const bool rotationChanged = m_hasPrev && std::memcmp(rot, m_prevRotation, sizeof(rot)) != 0;
    const bool invalidateAll = view.invalidateAll || rotationChanged;
    for (u32 l = 0; l < levels; ++l) {
        VsmLevelConstants& L = level[l];
        L.pageWorld = std::ldexp(pw0, static_cast<int>(l));
        L.invPageWorld = std::ldexp(inv0, -static_cast<int>(l));
        const f32 fx = std::floor(camLight[0] * L.invPageWorld);
        const f32 fy = std::floor(camLight[1] * L.invPageWorld);
        L.depthStep = L.pageWorld * 32.f; // a quarter window: the depth range re-centres in big steps
        const f32 fz = std::floor(camLight[2] * (L.invPageWorld * 0.03125f));
        const f32 limit = static_cast<f32>(core_logic::kVsmMaxOrigin);
        if (!(std::fabs(fx) <= limit) || !(std::fabs(fy) <= limit) || !(std::fabs(fz) <= limit)) {
            return false;
        }
        L.originX = static_cast<s32>(fx);
        L.originY = static_cast<s32>(fy);
        L.depthKey = static_cast<s32>(fz);
        L.depthCenter = static_cast<f32>(L.depthKey) * L.depthStep;
        L.prevOriginX = first ? L.originX : m_prevOrigin[l][0];
        L.prevOriginY = first ? L.originY : m_prevOrigin[l][1];
        L.prevDepthKey = first ? L.depthKey : m_prevDepthKey[l];
        L.flags = invalidateAll || L.prevDepthKey != L.depthKey ? 1u : 0u;
    }
    // depthToLight = [R 0; 0 1] * invViewProj (column-major), in double, rounded once.
    for (u32 col = 0; col < 4u; ++col) {
        for (u32 row = 0; row < 4u; ++row) {
            f64 s = 0.0;
            for (u32 k = 0; k < 4u; ++k) {
                const f64 r = row < 3u && k < 3u ? static_cast<f64>(rot[row * 3u + k]) : (row == 3u && k == 3u ? 1.0 : 0.0);
                s += r * static_cast<f64>(view.invViewProj[col * 4u + k]);
            }
            c.depthToLight[col * 4u + row] = static_cast<f32>(s);
        }
    }
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 k = 0; k < 3u; ++k) {
            c.lightRotation[r * 4u + k] = rot[r * 3u + k];
        }
        c.lightRotation[r * 4u + 3u] = 0.f;
        c.cameraLight[r] = camLight[r];
    }
    c.cameraLight[3] = 0.f;
    c.levelSelectScale = 1.f / (static_cast<f32>(kPagesPerAxis / 2u - 1u) * pw0);
    c.densityScale = view.pixelSpread / (m_desc.texelsPerPixel * (pw0 / static_cast<f32>(kPageTexels)));
    c.markRadiusPages = m_desc.markRadiusTexels / static_cast<f32>(kPageTexels);
    c.ndcScaleX = 2.f / static_cast<f32>(view.depthWidth);
    c.ndcScaleY = 2.f / static_cast<f32>(view.depthHeight);
    c.levels = levels;
    c.pagesPerAxis = kPagesPerAxis;
    c.pageTexels = kPageTexels;
    c.depthWidth = view.depthWidth;
    c.depthHeight = view.depthHeight;
    c.lodBias = m_desc.lodBias;
    c.frame = ++m_frame;
    c.flags = invalidateAll ? kFrameInvalidateAll : 0u;
    c.virtualPages = levels * kPagesPerLevel;
    c.requestWords = (c.virtualPages + 31u) / 32u;
    for (u32 l = 0; l < kMaxLevels; ++l) {
        c.level[l] = level[l];
    }
    std::memcpy(m_prevRotation, rot, sizeof(rot));
    for (u32 l = 0; l < levels; ++l) {
        m_prevOrigin[l][0] = level[l].originX;
        m_prevOrigin[l][1] = level[l].originY;
        m_prevDepthKey[l] = level[l].depthKey;
    }
    m_hasPrev = true;
    return true;
}

u32 markReference(const VsmFrameConstants& c, const f32* depth, u32 width, u32 height, u32* requestWords,
                  kernel::Backend backend, std::vector<u32>& scratch) {
    const u32 pixels = width * height;
    std::memset(requestWords, 0, static_cast<usize>(c.requestWords) * 4u);
    if (pixels == 0u) {
        return 0u;
    }
    if (scratch.size() < static_cast<usize>(pixels) * 4u) {
        scratch.resize(static_cast<usize>(pixels) * 4u);
    }
    mark_kernel::Params p{};
    p.constants = &c;
    p.depth = kernel::make_span(depth, pixels);
    p.width = width;
    p.pages = kernel::make_span(scratch.data(), pixels * 4u);
    kernel::launch(backend, mark_kernel::make_launch(width, height), mark_kernel::Kernel{}, p);
    u32 marked = 0;
    for (u32 i = 0; i < pixels * 4u; ++i) {
        const u32 v = scratch[i];
        if (v == kPageNone || v >= c.virtualPages) {
            continue;
        }
        u32& w = requestWords[v / 32u];
        const u32 bit = 1u << (v % 32u);
        marked += (w & bit) == 0u ? 1u : 0u;
        w |= bit;
    }
    return marked;
}

void setRectBits(u32 level, const s32 rect[4], u32* words) {
    const u32 base = level * kPagesPerLevel;
    for (s32 y = rect[1]; y <= rect[3]; ++y) {
        for (s32 x = rect[0]; x <= rect[2]; ++x) {
            const u32 v = base + kernel_math::slot_of(y) * kPagesPerAxis + kernel_math::slot_of(x);
            words[v / 32u] |= 1u << (v % 32u);
        }
    }
}

u32 VsmInvalidationReference::run(const VsmFrameConstants& c, const gpu_scene::GpuInstance* instances,
                                  const gpu_scene::GpuTransform* transforms, u32 instanceCount,
                                  const gpu_scene::GpuMesh* meshes, u32 meshCount, u32* invalidWords) {
    std::memset(invalidWords, 0, static_cast<usize>(c.requestWords) * 4u);
    m_cur.assign(instanceCount, VsmBoundsRecord{});
    if (instanceCount > 0u) {
        bounds_kernel::Params p{};
        p.instances = kernel::make_span(instances, instanceCount);
        p.transforms = kernel::make_span(transforms, instanceCount);
        p.meshes = kernel::make_span(meshes, meshCount);
        p.records = kernel::make_span(m_cur.data(), instanceCount);
        kernel::launch(kernel::Backend::CpuReference, bounds_kernel::make_launch(instanceCount), bounds_kernel::Kernel{}, p);
    }
    if (m_prev.size() < instanceCount) {
        m_prev.resize(instanceCount, VsmBoundsRecord{});
    }
    u32 changed = 0;
    for (u32 i = 0; i < instanceCount; ++i) {
        if (kernel_math::same_record(m_prev[i], m_cur[i])) {
            continue;
        }
        ++changed;
        for (u32 l = 0; l < c.levels; ++l) {
            s32 rect[4];
            if (kernel_math::bounds_rect(c, m_prev[i], l, rect)) {
                setRectBits(l, rect, invalidWords);
            }
            if (kernel_math::bounds_rect(c, m_cur[i], l, rect)) {
                setRectBits(l, rect, invalidWords);
            }
        }
        m_prev[i] = m_cur[i];
    }
    return changed;
}

core_logic::VsmFrameInput frameInput(const VsmFrameConstants& c) {
    core_logic::VsmFrameInput in{};
    in.frame = c.frame;
    in.levels = c.levels;
    in.invalidateAll = (c.flags & kFrameInvalidateAll) != 0u ? 1u : 0u;
    for (u32 l = 0; l < kMaxLevels; ++l) {
        in.level[l].originX = c.level[l].originX;
        in.level[l].originY = c.level[l].originY;
        in.level[l].depthKey = c.level[l].depthKey;
        in.level[l].pad = 0;
    }
    return in;
}

} // namespace fuse::renderer::vsm
