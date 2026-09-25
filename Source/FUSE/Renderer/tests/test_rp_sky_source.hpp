#pragma once
// Synthetic WP-8.2 sky source for the gates of the packages that sample the atmosphere LUTs through an optional
// AtParams address (WP-6.2 RT reflection misses, WP-6.3 SSR / SSGI sky fallback, WP-2.3 forward aerial
// perspective): an AtParams record (resolve_at_params over small LUT sizes, so the camera / sun frame fields are
// the real ones) followed by LUT sections filled with smooth deterministic patterns, written into one
// host-visible buffer. The kernels sample it with at_sample.{glsl,slang}; the CPU side evaluates the same
// sampling helpers (atmosphere_luts.hpp at_sky_radiance / at_aerial) on the same texels, so a gate compares the
// consumer's wiring (direction, distance, screen uv, flag) and not the LUT physics (WP-8.2's own gates).
//
//   SyntheticSky sky;
//   sky.build(cameraPosition, sunDirection, forward, right, up, tanHalfFovX, tanHalfFovY);
//   buffer of sky.bytes() (host-visible, BDA) -> sky.write(buffer.mapped, buffer.deviceAddress);
//   frame.atmosphereAddress = sky.address();   ...   sky.radiance(dir) / sky.aerial(u, v, distance, s, t)
#include <fuse/renderer/atmosphere/atmosphere_lut_types.hpp>
#include <fuse/renderer/atmosphere/atmosphere_luts.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstring>
#include <vector>

namespace fuse::renderer::test_sky {

struct SyntheticSky {
    atmosphere::AtParams params{};
    std::vector<atmosphere::AtTexel> transmittance;
    std::vector<atmosphere::AtTexel> multiscatter;
    std::vector<atmosphere::AtTexel> skyView;
    std::vector<atmosphere::AtTexel> aerialScatter;
    std::vector<atmosphere::AtTexel> aerialTransmittance;
    u64 base = 0; ///< device address of the record (0 until write)

    static constexpr u64 kRecordBytes = 512u; ///< AtParams (320 B) rounded up; LUT sections follow, 256-aligned

    bool build(const math::Vec3& cameraPosition, const math::Vec3& toSun, const math::Vec3& forward, const math::Vec3& right,
               const math::Vec3& up, f32 tanHalfFovX, f32 tanHalfFovY, f32 aerialMaxDistance = 64.f) {
        atmosphere::AtmosphereLutSettings s{};
        s.sizes.transWidth = 16;
        s.sizes.transHeight = 8;
        s.sizes.msWidth = 8;
        s.sizes.msHeight = 8;
        s.sizes.skyWidth = 32;
        s.sizes.skyHeight = 18;
        s.sizes.apWidth = 8;
        s.sizes.apHeight = 8;
        s.sizes.apDepth = 8;
        atmosphere::AtmosphereLutView v{};
        v.cameraPosition = cameraPosition;
        v.sunDirection = toSun;
        v.sunIlluminance = math::Vec3{2.f, 1.8f, 1.5f};
        v.forward = forward;
        v.right = right;
        v.up = up;
        v.tanHalfFovX = tanHalfFovX;
        v.tanHalfFovY = tanHalfFovY;
        v.aerialMaxDistance = aerialMaxDistance;
        if (!atmosphere::resolve_at_params(s, v, params)) {
            return false;
        }
        auto fill = [](std::vector<atmosphere::AtTexel>& t, u32 w, u32 h, u32 d, f32 a, f32 b, f32 c, f32 offset) {
            t.resize(static_cast<usize>(w) * h * d);
            for (u32 z = 0; z < d; ++z) {
                for (u32 y = 0; y < h; ++y) {
                    for (u32 x = 0; x < w; ++x) {
                        const f32 fx = static_cast<f32>(x) / static_cast<f32>(w - 1u);
                        const f32 fy = static_cast<f32>(y) / static_cast<f32>(h - 1u);
                        const f32 fz = d > 1u ? static_cast<f32>(z) / static_cast<f32>(d - 1u) : 0.f;
                        atmosphere::AtTexel& e = t[(static_cast<usize>(z) * h + y) * w + x];
                        e.r = offset + a * fx + 0.1f * fz;
                        e.g = offset + b * fy + 0.05f * fx * fy + 0.1f * fz;
                        e.b = offset + c * (1.f - fx) * (1.f - 0.5f * fy) + 0.1f * fz;
                        e.a = 0.f;
                    }
                }
            }
        };
        const atmosphere::AtParams& p = params;
        fill(transmittance, p.transWidth, p.transHeight, 1u, 0.1f, 0.1f, 0.1f, 0.7f);
        fill(multiscatter, p.msWidth, p.msHeight, 1u, 0.01f, 0.01f, 0.01f, 0.01f);
        fill(skyView, p.skyWidth, p.skyHeight, 1u, 0.4f, 0.3f, 0.5f, 0.05f);
        fill(aerialScatter, p.apWidth, p.apHeight, p.apDepth, 0.02f, 0.03f, 0.04f, 0.01f);
        fill(aerialTransmittance, p.apWidth, p.apHeight, p.apDepth, -0.1f, -0.15f, -0.05f, 0.8f);
        return true;
    }

    u64 bytes() const {
        return kRecordBytes + section(transmittance) + section(multiscatter) + section(skyView) + section(aerialScatter) +
               section(aerialTransmittance);
    }

    /// Writes the record + LUT sections at `mapped` (device address `address`); the record's LUT addresses point
    /// into the same buffer.
    void write(void* mapped, u64 address) {
        base = address;
        u8* dst = static_cast<u8*>(mapped);
        u64 cursor = kRecordBytes;
        auto put = [&](const std::vector<atmosphere::AtTexel>& t) {
            std::memcpy(dst + cursor, t.data(), t.size() * sizeof(atmosphere::AtTexel));
            const u64 at = address + cursor;
            cursor += section(t);
            return at;
        };
        params.transmittance = put(transmittance);
        params.multiscatter = put(multiscatter);
        params.skyView = put(skyView);
        params.aerialScatter = put(aerialScatter);
        params.aerialTransmittance = put(aerialTransmittance);
        std::memcpy(dst, &params, sizeof(params));
    }

    u64 address() const { return base; }

    atmosphere::AtLutView view() const {
        atmosphere::AtLutView v{};
        v.transmittance = transmittance.data();
        v.multiscatter = multiscatter.data();
        v.skyView = skyView.data();
        v.aerialScatter = aerialScatter.data();
        v.aerialTransmittance = aerialTransmittance.data();
        return v;
    }

    /// at_sky_radiance(params, dir, sunDisk) on the CPU (x sunIlluminance).
    math::Vec3 radiance(const math::Vec3& dir, bool sunDisk = false) const {
        return atmosphere::at_sky_radiance(params, view(), dir, sunDisk);
    }

    /// at_aerial at screen uv (y down) and view distance, and the sun illuminance it is scaled by.
    void aerial(f32 u, f32 v, f32 distance, math::Vec3& scatter, math::Vec3& trans) const {
        atmosphere::at_aerial(params, view(), u, v, distance, scatter, trans);
    }
    math::Vec3 illuminance() const {
        return math::Vec3{params.sunIlluminance[0], params.sunIlluminance[1], params.sunIlluminance[2]};
    }

private:
    static u64 section(const std::vector<atmosphere::AtTexel>& t) {
        return (static_cast<u64>(t.size()) * sizeof(atmosphere::AtTexel) + 255u) & ~u64{255u};
    }
};

} // namespace fuse::renderer::test_sky
