// FUSE Relight: Remix-compatible light hashes (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1.4).
//
// Bit-exact with the RtLight*::updateCachedHash functions of dxvk-remix @0867d3c
// (src/dxvk/rtx_render/rtx_lights.cpp). The inputs are the RtLight fields after Remix's
// D3DLIGHT9 -> RtLight conversion (LightData / LightUtils, ported with the capture work packages);
// radiance and volumetric scale are not hashed upstream.
#pragma once

#include <fuse/relight/hash/xxh.hpp>

#include <array>
#include <cstdint>

namespace fuse::relight::hash {

/// RtLightType (lightType* constants of the light shaders).
enum class LightType : std::uint32_t {
    Sphere = 0,
    Rect = 1,
    Disk = 2,
    Cylinder = 3,
    Distant = 4,
};

using Float2 = std::array<float, 2>;
using Float3 = std::array<float, 3>;

/// RtLightShaping.
struct LightShaping {
    bool enabled = false;
    Float3 direction{0.f, 0.f, 1.f};
    float cosConeAngle = 0.f;
    float coneSoftness = 0.f;
    float focusExponent = 0.f;
};

/// RtLightShaping::getHash: 0 when disabled, else XXH64 chain over direction (12 bytes),
/// cosConeAngle, coneSoftness and focusExponent, seeded from 0.
[[nodiscard]] Hash64 hashLightShaping(const LightShaping& shaping) noexcept;

/// RtSphereLight: h = Sphere; XXH64(position, 12, h); XXH64(&radius, 4, h); XXH64(&h, 8, shapingHash).
[[nodiscard]] Hash64 hashSphereLight(const Float3& position, float radius, const LightShaping& shaping) noexcept;

/// RtRectLight: position, dimensions (8 bytes), xAxis, yAxis, direction, then the shaping fold.
[[nodiscard]] Hash64 hashRectLight(const Float3& position, const Float2& dimensions, const Float3& xAxis, const Float3& yAxis,
                                   const Float3& direction, const LightShaping& shaping) noexcept;

/// RtDiskLight: position, halfDimensions (8 bytes), xAxis, yAxis, direction, then the shaping fold.
[[nodiscard]] Hash64 hashDiskLight(const Float3& position, const Float2& halfDimensions, const Float3& xAxis,
                                   const Float3& yAxis, const Float3& direction, const LightShaping& shaping) noexcept;

/// RtCylinderLight: position, radius, axis, axisLength (no shaping).
[[nodiscard]] Hash64 hashCylinderLight(const Float3& position, float radius, const Float3& axis, float axisLength) noexcept;

/// RtDistantLight: direction, halfAngle (no shaping).
[[nodiscard]] Hash64 hashDistantLight(const Float3& direction, float halfAngle) noexcept;

} // namespace fuse::relight::hash
