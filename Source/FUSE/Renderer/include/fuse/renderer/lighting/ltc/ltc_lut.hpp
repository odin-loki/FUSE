#pragma once

// WP-2.2 host side of the BRDF / LTC look-up tables and the area-light rows (see ltc_kernel.hpp for the
// layout and the math).
//
//   BrdfLut lut;
//   bakeBrdfLut(lut);                     // LTC table copied, DFG + sphere tables baked (CpuParallel)
//   lut.data()                            // kLutWords f32: upload once (ClusteredLighting does) or hand to
//                                         // the CPU references (ShadeReferenceDesc::brdfLut)
//   scene.addLight(makeRectLight(...));   // GpuLight rows of the new light types

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/lighting/ltc/ltc_kernel.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::ltc {

/// The fitted LTC table (ltc_lut_data.inc): kLtcSize * kLtcSize * 4 f32.
const f32* ltcTable();

class BrdfLut {
public:
    const f32* data() const { return m_words.empty() ? nullptr : m_words.data(); }
    u32 size() const { return static_cast<u32>(m_words.size()); }
    u64 bytes() const { return static_cast<u64>(m_words.size()) * sizeof(f32); }
    bool valid() const { return m_words.size() == kLutWords; }
    kernel::Span<const f32> span() const { return kernel::Span<const f32>{data(), size()}; }

private:
    friend bool bakeBrdfLut(BrdfLut& lut, kernel::Backend backend);
    std::vector<f32> m_words;
};

/// Copies the LTC table and runs the "brdf_lut.dfg" and "brdf_lut.sphere" kernels on `backend`
/// (CpuReference and CpuParallel are bit-identical). False when a launch fails.
bool bakeBrdfLut(BrdfLut& lut, kernel::Backend backend = kernel::Backend::CpuParallel);

/// Process-wide baked table (baked once on first use, CpuParallel; thread-safe).
const BrdfLut& sharedBrdfLut();

/// Octahedral snorm16x2 encoding of a unit vector (GpuLight::flags of rectangle lights).
u32 encodeTangent(const math::Vec3& unit);
/// Decoder (FUSE_HOST_DEVICE twin: decode_tangent in ltc_kernel.hpp's users; exposed for tests).
math::Vec3 decodeTangent(u32 bits);

struct AreaLightDesc {
    math::Vec3 center{};
    math::Vec3 normal{0.f, 0.f, -1.f};  ///< lit side
    math::Vec3 tangent{1.f, 0.f, 0.f};  ///< rectangle width axis (orthogonalised against the normal)
    f32 halfWidth = 0.5f;               ///< rectangle half width; disk: radius along the tangent
    f32 halfHeight = 0.5f;              ///< rectangle half height; disk: radius along normal x tangent (= halfWidth: a circle)
    math::Vec3 color{1.f, 1.f, 1.f};
    f32 intensity = 1.f;                ///< emitted radiance = colour x intensity
    f32 range = 10.f;                   ///< influence radius around the centre (clustering, window)
};

gpu_scene::GpuLight makeRectLight(const AreaLightDesc& d);
gpu_scene::GpuLight makeDiskLight(const AreaLightDesc& d);
/// Gives a directional light an angular radius (radians; 0 = punctual).
void setSunAngularRadius(gpu_scene::GpuLight& light, f32 radians);

} // namespace fuse::renderer::ltc
