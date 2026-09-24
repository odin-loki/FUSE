// WP-2.2 BRDF / LTC look-up tables and area-light rows: see include/fuse/renderer/lighting/ltc/ltc_lut.hpp.
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::renderer::ltc {

namespace {

// The fitted table as IEEE-754 binary32 words (provenance and licence in the file header).
alignas(16) constexpr u32 kLtcWords[] = {
#include "ltc_lut_data.inc"
};
static_assert(sizeof(kLtcWords) == kLtcSize * kLtcSize * 4u * sizeof(u32), "ltc_lut_data.inc size");

struct LtcTable {
    f32 values[kLtcSize * kLtcSize * 4u];
    LtcTable() { std::memcpy(values, kLtcWords, sizeof(values)); }
};

u32 snormWord(f32 v) {
    const f32 c = std::clamp(v, -1.f, 1.f);
    const i32 q = static_cast<i32>(std::lround(c * 32767.f));
    return static_cast<u32>(q) & 0xFFFFu;
}

} // namespace

const f32* ltcTable() {
    static const LtcTable table;
    return table.values;
}

bool bakeBrdfLut(BrdfLut& lut, kernel::Backend backend) {
    lut.m_words.assign(kLutWords, 0.f);
    std::memcpy(lut.m_words.data() + kLtcOffset, ltcTable(), kLtcSize * kLtcSize * 4u * sizeof(f32));
    const BakeParams params{lut.m_words.data()};
    const kernel::LaunchResult dfg = kernel::launch(backend, make_dfg_bake_launch(), DfgBakeKernel{}, params);
    const kernel::LaunchResult sphere = kernel::launch(backend, make_sphere_bake_launch(), SphereBakeKernel{}, params);
    if (!dfg.ok || !sphere.ok) {
        lut.m_words.clear();
        return false;
    }
    return true;
}

const BrdfLut& sharedBrdfLut() {
    static const BrdfLut lut = [] {
        BrdfLut l;
        bakeBrdfLut(l, kernel::Backend::CpuParallel);
        return l;
    }();
    return lut;
}

u32 encodeTangent(const math::Vec3& unit) {
    const f32 sum = std::fabs(unit.x) + std::fabs(unit.y) + std::fabs(unit.z);
    const f32 inv = sum > 0.f ? 1.f / sum : 0.f;
    f32 ox = unit.x * inv;
    f32 oy = unit.y * inv;
    if (unit.z < 0.f) {
        const f32 x = (1.f - std::fabs(oy)) * (ox >= 0.f ? 1.f : -1.f);
        const f32 y = (1.f - std::fabs(ox)) * (oy >= 0.f ? 1.f : -1.f);
        ox = x;
        oy = y;
    }
    return snormWord(ox) | (snormWord(oy) << 16u);
}

math::Vec3 decodeTangent(u32 bits) { return decode_tangent(bits); }

namespace {
gpu_scene::GpuLight areaRow(const AreaLightDesc& d, u32 type) {
    gpu_scene::GpuLight l{};
    l.type = type;
    l.position[0] = d.center.x;
    l.position[1] = d.center.y;
    l.position[2] = d.center.z;
    const math::Vec3 n = safe_unit(d.normal, math::Vec3{0.f, 0.f, -1.f});
    l.direction[0] = n.x;
    l.direction[1] = n.y;
    l.direction[2] = n.z;
    l.range = d.range;
    l.color[0] = d.color.x;
    l.color[1] = d.color.y;
    l.color[2] = d.color.z;
    l.intensity = d.intensity;
    l.cosInner = d.halfWidth;
    l.cosOuter = d.halfHeight;
    math::Vec3 ex{};
    math::Vec3 ey{};
    area_axes(n, d.tangent, 1.f, 1.f, ex, ey);
    l.flags = encodeTangent(ex);
    return l;
}
} // namespace

gpu_scene::GpuLight makeRectLight(const AreaLightDesc& d) { return areaRow(d, kLightRect); }

gpu_scene::GpuLight makeDiskLight(const AreaLightDesc& d) { return areaRow(d, kLightDisk); }

void setSunAngularRadius(gpu_scene::GpuLight& light, f32 radians) {
    light.cosOuter = radians > 0.f ? std::cos(radians) : 1.f;
}

} // namespace fuse::renderer::ltc
